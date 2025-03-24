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
    socklen_t bind_addrlen = socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr), this->port_);

    this->socket_ = socket::socket_ip(SOCK_STREAM, AF_INET);
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
void LOG_BYTES(const char *tag, const char *prefix, const uint8_t *data, size_t len) {
    char buf[512]; // Ensure this buffer is large enough for your data
    size_t pos = 0;

    for (size_t i = 0; i < len && pos < sizeof(buf) - 3; i++) { // Reserve space for null terminator
        pos += snprintf(&buf[pos], sizeof(buf) - pos, "%02X:", data[i]);
    }
    buf[pos] = '\0'; // Null-terminate the string
    ESP_LOGD(tag, "%s %s", prefix, buf);
}

void StreamServerComponent::exchange() 
{
    uint8_t socket_buf[260]; // Buffer for reading socket data
    uint8_t uart_buf[260];   // Buffer for UART response
    ssize_t socket_read_len;
    ssize_t uart_available;

    // First see if we have a client waiting for a reponse from the UART
    for (Client &client : this->clients_) 
    {
        if (client.disconnected)        // this client is disconneted -> skip
            continue;

        if (!client.uart_user_)         // this client is not waiting for a response -> skip
            continue;
        
        // found a client awaiting for UART response
        uart_available = this->stream_->available();

        if (last_uart_availability_ != uart_available) { // data is still coming in
            last_uart_availability_ = uart_available;
            client.last_uart_time = esphome::millis(); // there is data comming in
            esphome::delay(15); // wait for more data to come in
        }
        else if (uart_available > 3) // wait for at least 4 bytes to be available
        {
            if (uart_available > sizeof(uart_buf)) { // buffer overflow protection
                ESP_LOGW(TAG, "UART buffer overflow, discarding %d bytes", uart_available - sizeof(uart_buf));
                uart_available = sizeof(uart_buf);
            }
            if (this->stream_->read_array(uart_buf, uart_available) == false) {
                ESP_LOGW(TAG, "Failed to read from UART");
                client.uart_user_ = false;
                this->stream_->flush();
                last_uart_availability_ = 0; // reset the availability counter
                continue;
            }
            // Step 4: Send the UART response back to the socket
            if (this->modbus_) {
                this->modbus_rtu_to_tcp(uart_buf, uart_available);
            }
            LOG_BYTES(TAG, "To send >>>", uart_buf, uart_available);
            int written = client.socket->write(uart_buf, uart_available);
            ESP_LOGI(TAG, "UART response of %d bytes sent to client %s", written, client.identifier.c_str());

            // Clear the current client and reset the timer
            client.last_uart_time = esphome::millis(); // Reset the timeout timer            
            client.uart_user_ = false;
        } 
        if (esphome::millis() - client.last_uart_time > 5000) { // 5-second ModBus timeout
            // Handle UART timeout
            ESP_LOGW(TAG, "UART response timeout for client %s", client.identifier.c_str());
            // flush all remaining bytes in UART queue and hope to recover
            this->stream_->flush();
            last_uart_availability_ = 0; // reset the availability counter
            client.uart_user_ = false;
        }
        if (client.uart_user_)  // this client is still actively waiting for uart response
            return;     // skip the rest of the loop, and skip sending any data
    }

    // No client is waiting for a response, so we can read from the socket to send new data to uart
    for (Client &client : this->clients_) 
    {
        if (client.disconnected)
            continue;

        socket_read_len = client.socket->read(socket_buf, sizeof(socket_buf));
        if (socket_read_len > 0) 
        {
            LOG_BYTES(TAG, "Received <<<", socket_buf, socket_read_len);
            // Step 2: Send the data to the UART
            if (this->modbus_) {
                this->modbus_tcp_to_rtu(socket_buf, socket_read_len);
            }
            this->stream_->flush(); // empty UART as we will write new data
            last_uart_availability_ = 0; // reset the availability counter
            this->stream_->write_array(socket_buf, socket_read_len);

            // Mark the client as waiting for a UART response
            client.last_uart_time = esphome::millis(); // Start the timeout timer
            client.uart_user_ = true;
            
            return; // we now wait for the UART response
        } 
        if (socket_read_len == 0 || errno == ECONNRESET) 
        {
            // Handle socket disconnection
            ESP_LOGD(TAG, "Client %s disconnected", client.identifier.c_str());
            client.disconnected = true;
            continue;
        } 
        // socket_read_len < 0
        if (errno == EWOULDBLOCK || errno == EAGAIN) {  
            // No data available on the socket
            // cleanup clients which used the uart once, and have no communication for 60 seconds
            if (esphome::millis() - client.last_uart_time > 60000) {
                ESP_LOGD(TAG, "Client %s disconnected due to inactivity", client.identifier.c_str());
                client.disconnected = true;
            }
        } else {
            ESP_LOGW(TAG, "Failed to read from client %s with error %d!", client.identifier.c_str(), errno);
            client.disconnected = true;
        }
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
StreamServerComponent::Client::Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier)
    : socket(std::move(socket)), identifier{identifier} 
{
    last_uart_time = esphome::millis(); // give it a fresh start
    uart_user_ = false;
    disconnected = false;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
//  Some helpers to convert Modbus TCP to RTU and vice versa
/////////////////////////////////////////////////////////////////////////////////////////////////////////////
void StreamServerComponent::modbus_tcp_to_rtu(uint8_t *frame, ssize_t &len) 
{
    // Modbus TCP to RTU conversion logic
    // Example: Strip the MBAP header (first 7 bytes) and add CRC
    if (len < 8) {
        len = 0;
        ESP_LOGE(TAG, "Frame too short for Modbus TCP conversion");
        return;
    }
    this->last_transaction_id_ = (frame[0] << 8) | frame[1];
    this->last_protocol_id_ = (frame[2] << 8) | frame[3];
    ssize_t frame_len = (frame[4] << 8) | frame[5];
    if (len < frame_len + 6) {
        len = 0;
        ESP_LOGE(TAG, "Invalid Modbus TCP frame length");
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
    // TODO validate CRC and frame len
    uint16_t transaction_id = this->last_transaction_id_;
    uint16_t protocol_id = this->last_protocol_id_;
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