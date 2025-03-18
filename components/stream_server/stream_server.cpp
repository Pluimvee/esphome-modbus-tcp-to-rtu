#include "stream_server.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/util.h"
#include "esphome/core/version.h"

#include "esphome/components/network/util.h"
#include "esphome/components/socket/socket.h"

static const char *TAG = "stream_server";

using namespace esphome;

void StreamServerComponent::setup() {
    ESP_LOGCONFIG(TAG, "Setting up stream server...");

    // The make_unique() wrapper doesn't like arrays, so initialize the unique_ptr directly.
    this->buf_ = std::unique_ptr<uint8_t[]>{new uint8_t[this->buf_size_]};

    struct sockaddr_storage bind_addr;
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2023, 4, 0)
    socklen_t bind_addrlen = socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr), this->port_);
#else
    socklen_t bind_addrlen = socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr), htons(this->port_));
#endif

    this->socket_ = socket::socket_ip(SOCK_STREAM, PF_INET);
    this->socket_->setblocking(false);
    this->socket_->bind(reinterpret_cast<struct sockaddr *>(&bind_addr), bind_addrlen);
    this->socket_->listen(8);

    this->publish_sensor();
}

void StreamServerComponent::loop() {
    this->accept();
    this->read();
    this->flush();
    this->write();
    this->cleanup();
}

void StreamServerComponent::dump_config() {
    ESP_LOGCONFIG(TAG, "Stream Server:");
    ESP_LOGCONFIG(TAG, "  Address: %s:%u", esphome::network::get_use_address().c_str(), this->port_);
#ifdef USE_BINARY_SENSOR
    LOG_BINARY_SENSOR("  ", "Connected:", this->connected_sensor_);
#endif
#ifdef USE_SENSOR
    LOG_SENSOR("  ", "Connection count:", this->connection_count_sensor_);
#endif
}

void StreamServerComponent::on_shutdown() {
    for (const Client &client : this->clients_)
        client.socket->shutdown(SHUT_RDWR);
}

void StreamServerComponent::publish_sensor() {
#ifdef USE_BINARY_SENSOR
    if (this->connected_sensor_)
        this->connected_sensor_->publish_state(this->clients_.size() > 0);
#endif
#ifdef USE_SENSOR
    if (this->connection_count_sensor_)
        this->connection_count_sensor_->publish_state(this->clients_.size());
#endif
}

void StreamServerComponent::accept() {
    struct sockaddr_storage client_addr;
    socklen_t client_addrlen = sizeof(client_addr);
    std::unique_ptr<socket::Socket> socket = this->socket_->accept(reinterpret_cast<struct sockaddr *>(&client_addr), &client_addrlen);
    if (!socket)
        return;

    socket->setblocking(false);
    std::string identifier = socket->getpeername();
    this->clients_.emplace_back(std::move(socket), identifier, this->buf_head_);
    ESP_LOGD(TAG, "New client connected from %s", identifier.c_str());
    this->publish_sensor();
}

void StreamServerComponent::cleanup() {
    auto discriminator = [](const Client &client) { return !client.disconnected; };
    auto last_client = std::partition(this->clients_.begin(), this->clients_.end(), discriminator);
    if (last_client != this->clients_.end()) {
        this->clients_.erase(last_client, this->clients_.end());
        this->publish_sensor();
    }
}

// read from uart
void StreamServerComponent::read() {
    size_t len = 0;
    int available;
    while ((available = this->stream_->available()) > 0) {
        size_t free = this->buf_size_ - (this->buf_head_ - this->buf_tail_);
        if (free == 0) {
            // Only overwrite if nothing has been added yet, otherwise give flush() a chance to empty the buffer first.
            if (len > 0)
                return;

            ESP_LOGE(TAG, "Incoming bytes available, but outgoing buffer is full: stream will be corrupted!");
            free = std::min<size_t>(available, this->buf_size_);
            this->buf_tail_ += free;
            for (Client &client : this->clients_) {
                if (client.position < this->buf_tail_) {
                    ESP_LOGW(TAG, "Dropped %u pending bytes for client %s", this->buf_tail_ - client.position, client.identifier.c_str());
                    client.position = this->buf_tail_;
                }
            }
        }
        // Fill all available contiguous space in the ring buffer.
        len = std::min<size_t>(available, std::min<size_t>(this->buf_ahead(this->buf_head_), free));
        this->stream_->read_array(&this->buf_[this->buf_index(this->buf_head_)], len);
        this->buf_head_ += len;
    }
}

// flush data to clients
void StreamServerComponent::flush() {
    ssize_t written;
    this->buf_tail_ = this->buf_head_;
    for (Client &client : this->clients_) {
        if (client.disconnected || client.position == this->buf_head_)
            continue;

        // Split the write into two parts: from the current position to the end of the ring buffer, and from the start
        // of the ring buffer until the head. The second part might be zero if no wraparound is necessary.
        struct iovec iov[2];
        iov[0].iov_base = &this->buf_[this->buf_index(client.position)];
        iov[0].iov_len = std::min(this->buf_head_ - client.position, this->buf_ahead(client.position));
        iov[1].iov_base = &this->buf_[0];
        iov[1].iov_len = this->buf_head_ - (client.position + iov[0].iov_len);

        written = iov[0].iov_len + iov[1].iov_len;
        // conversion needed?
        uint8_t tcp_frame[270];     // defined here to ensure pointer remains valid
        ssize_t tcp_len = 0;
        if (modbus_)
        {
            // Copy data from the ring buffer into a temporary buffer
            if (written > sizeof(tcp_frame)) {
                ESP_LOGE(TAG, "Frame to large, cannot convert Modbus RTU to TCP!");
                return;
            }
            if (iov[0].iov_len > 0) {
                memcpy(tcp_frame, iov[0].iov_base, iov[0].iov_len);
                tcp_len += iov[0].iov_len;
            }
            if (iov[1].iov_len > 0) {
                memcpy(tcp_frame + tcp_len, iov[1].iov_base, iov[1].iov_len);
                tcp_len += iov[1].iov_len;
            }
            // Perform the conversion 
            this->convert_modbus_rtu_to_tcp(tcp_frame, tcp_len);
            iov[0].iov_base = tcp_frame;
            iov[0].iov_len = tcp_len;
            iov[1].iov_len = 0; // No second part after conversion
        }
        if ((client.socket->writev(iov, 2)) > 0) {
            client.position += written;
        } else if (written == 0 || errno == ECONNRESET) {
            ESP_LOGD(TAG, "Client %s disconnected", client.identifier.c_str());
            client.disconnected = true;
            continue;  // don't consider this client when calculating the tail position
        } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
            // Expected if the (TCP) transmit buffer is full, nothing to do.
        } else {
            ESP_LOGE(TAG, "Failed to write to client %s with error %d!", client.identifier.c_str(), errno);
        }
        this->buf_tail_ = std::min(this->buf_tail_, client.position);
    }
}

// write to uart after reading from clients
void StreamServerComponent::write() 
{
    uint8_t buf[260]; // Increase buffer size to 260 bytes
    ssize_t read;
    for (Client &client : this->clients_) 
    {
        if (client.disconnected)
            continue;

        while ((read = client.socket->read(&buf, sizeof(buf))) > 0) {
            if (this->modbus_)
                this->convert_modbus_tcp_to_rtu(buf, read);
            this->stream_->write_array(buf, read);
        }

        if (read == 0 || errno == ECONNRESET) {
            ESP_LOGD(TAG, "Client %s disconnected", client.identifier.c_str());
            client.disconnected = true;
        } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
            // Expected if the (TCP) receive buffer is empty, nothing to do.
        } else {
            ESP_LOGW(TAG, "Failed to read from client %s with error %d!", client.identifier.c_str(), errno);
        }
    }
}

StreamServerComponent::Client::Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier, size_t position)
    : socket(std::move(socket)), identifier{identifier}, position{position} {}

void StreamServerComponent::convert_modbus_tcp_to_rtu(uint8_t *frame, ssize_t &len) 
{
    // Modbus TCP to RTU conversion logic
    // Example: Strip the MBAP header (first 7 bytes) and add CRC
    if (len < 7) {
        len = 0;
        return;
    }
    memmove(frame, frame +6, len -6); // Shift TCP frame to remove MBAP header
    len -= 6;
    uint16_t crc = calculate_crc(frame, len);
    frame[len++] = crc & 0xFF;
    frame[len++] = (crc >> 8) & 0xFF;
}

void StreamServerComponent::convert_modbus_rtu_to_tcp(uint8_t *frame, ssize_t &len) 
{
    // Modbus RTU to TCP conversion logic -> Add MBAP header and strip CRC
    if (len < 4) {
        len = 0;
        return;
    }
    uint16_t transaction_id = 0; // Set appropriate transaction ID
    uint16_t protocol_id = 0;
    uint16_t length = len -2; // Length without CRC
    memmove(frame + 6, frame, len - 2); // Shift RTU frame to make space for MBAP header,
    frame[0] = (transaction_id >> 8) & 0xFF;
    frame[1] = transaction_id & 0xFF;
    frame[2] = (protocol_id >> 8) & 0xFF;
    frame[3] = protocol_id & 0xFF;
    frame[4] = (length >> 8) & 0xFF;
    frame[5] = length & 0xFF;
    len = 6 + len - 2;             //  Note: buffer needs to be 4 bytes longer as len
}

uint16_t StreamServerComponent::calculate_crc(const uint8_t *data, size_t &len) {
    // Implement CRC calculation for Modbus RTU
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}