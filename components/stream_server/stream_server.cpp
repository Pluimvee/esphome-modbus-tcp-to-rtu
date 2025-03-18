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

    struct sockaddr_storage bind_addr;
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2023, 4, 0)
    socklen_t bind_addrlen = socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr), 502); //this->port_);
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
    this->exchange();
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

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Accepting new connections
/////////////////////////////////////////////////////////////////////////////////////////////////////////////
void StreamServerComponent::accept() 
{
    struct sockaddr_storage client_addr;
    socklen_t client_addrlen = sizeof(client_addr);
    std::unique_ptr<socket::Socket> socket = this->socket_->accept(reinterpret_cast<struct sockaddr *>(&client_addr), &client_addrlen);
    if (!socket)
        return;

    socket->setblocking(false);
    std::string identifier = socket->getpeername();
    this->clients_.emplace_back(std::move(socket), identifier);
    ESP_LOGD(TAG, "New client connected from %s", identifier.c_str());
    this->publish_sensor();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Cleanup closed connections
/////////////////////////////////////////////////////////////////////////////////////////////////////////////
void StreamServerComponent::cleanup() 
{
    auto discriminator = [](const Client &client) { return !client.disconnected; };
    auto last_client = std::partition(this->clients_.begin(), this->clients_.end(), discriminator);
    if (last_client != this->clients_.end()) {
        this->clients_.erase(last_client, this->clients_.end());
        this->publish_sensor();
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Exchange messages from socket to UART and back
/////////////////////////////////////////////////////////////////////////////////////////////////////////////
void StreamServerComponent::exchange() 
{
    uint8_t socket_buf[260]; // Buffer for reading socket data
    uint8_t uart_buf[260];   // Buffer for UART response
    ssize_t socket_read_len;
    ssize_t uart_read_len;

    for (Client &client : this->clients_) {
        if (client.disconnected)
            continue;

        // Step 1: Read data from the socket
        if (current_client_ == nullptr) { // Only read if no client is waiting for a response
            socket_read_len = client.socket->read(socket_buf, sizeof(socket_buf));
            if (socket_read_len > 0) {
                // Step 2: Send the data to the UART
                if (this->modbus_) {
                    this->modbus_tcp_to_rtu(socket_buf, socket_read_len);
                }
                this->stream_->write_array(socket_buf, socket_read_len);

                // Mark the client as waiting for a UART response
                current_client_ = &client;
                client.last_uart_time = esphome::millis(); // Start the timeout timer
            
                // Enforce a minimum wait time before reading from the UART
                esphome::delay(100);
                return; // Move to the next iteration to wait for the UART response
            } 
            if (socket_read_len == 0 || errno == ECONNRESET) {
                // Handle socket disconnection
                ESP_LOGD(TAG, "Client %s disconnected", client.identifier.c_str());
                client.disconnected = true;

                return;
            } 
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                // No data available on the socket, continue to the next client
                return;
                
            } else {
                ESP_LOGW(TAG, "Failed to read from client %s with error %d!", client.identifier.c_str(), errno);
                client.disconnected = true;
            }
            return;
        }

        // Step 2: Wait for UART response (non-blocking)
        if (current_client_ == &client) 
        {
            uart_read_len = this->stream_->available();

            if (uart_read_len > 5) {
                uart_read_len = this->stream_->read_array(uart_buf, std::min(uart_read_len, (ssize_t) sizeof(uart_buf)));

                // Step 4: Send the UART response back to the socket
                if (this->modbus_) {
                    this->modbus_rtu_to_tcp(uart_buf, uart_read_len);
                }
                client.socket->write(uart_buf, uart_read_len);

                // Clear the current client and reset the timer
                current_client_ = nullptr;
            } else if (esphome::millis() - client.last_uart_time > 5000) { // 5-second ModBus timeout
                // Handle UART timeout
                ESP_LOGW(TAG, "UART response timeout for client %s", client.identifier.c_str());
                current_client_ = nullptr;
                // flush all remaining bytes in UART queue and hope to recover
                this->stream_->flush();
            }
        }
        else {
            // cleanup clients which have no communicaiton for 60 seconds
            if (esphome::millis() - client.last_uart_time > 60000) {
                ESP_LOGD(TAG, "Client %s disconnected due to inactivity", client.identifier.c_str());
                client.socket->close();
                client.disconnected = true;
            }
            return;
        }
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
StreamServerComponent::Client::Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier)
    : socket(std::move(socket)), identifier{identifier} {}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
//  Some helpers to convert Modbus TCP to RTU and vice versa
/////////////////////////////////////////////////////////////////////////////////////////////////////////////
void StreamServerComponent::modbus_tcp_to_rtu(uint8_t *frame, ssize_t &len) 
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

void StreamServerComponent::modbus_rtu_to_tcp(uint8_t *frame, ssize_t &len) 
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

uint16_t StreamServerComponent::calculate_crc(const uint8_t *data, size_t len) {
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