#include "modbus_bridge.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/util.h"
#include "esphome/core/version.h"

#include "esphome/components/network/util.h"
#include "esphome/components/socket/socket.h"

static const char *TAG = "ModBus_bridge";

using namespace esphome;

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
// ModBusBridgeComponent implementation
/////////////////////////////////////////////////////////////////////////////////////////////////////////////
void ModBusBridgeComponent::setup() {
    ESP_LOGCONFIG(TAG, "Setting up ModBus bridge...");

    struct sockaddr_storage bind_addr;
    socklen_t bind_addrlen = socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr), this->port_);

    this->listener_ = socket::socket_ip(SOCK_STREAM, AF_INET);
    this->listener_->setblocking(false);
    this->listener_->bind(reinterpret_cast<struct sockaddr *>(&bind_addr), bind_addrlen);
    this->listener_->listen(8);

    this->publish_sensor();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
void ModBusBridgeComponent::loop() {
    this->accept();
    this->read();
    this->exchange();
    this->cleanup();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
void ModBusBridgeComponent::dump_config() {
    ESP_LOGCONFIG(TAG, "ModBus Bridge:");
    ESP_LOGCONFIG(TAG, "  Address: %s:%u", esphome::network::get_use_address().c_str(), this->port_);
    ESP_LOGCONFIG(TAG, "  ModBus timeout: %d ms", this->timeout_);
    ESP_LOGCONFIG(TAG, "  UART buffer: %d bytes", this->buf_size_);
#ifdef USE_BINARY_SENSOR
    LOG_BINARY_SENSOR("  ", "Connected:", this->connected_sensor_);
#endif
#ifdef USE_SENSOR
    LOG_SENSOR("  ", "Connection count:", this->connection_count_sensor_);
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
void ModBusBridgeComponent::on_shutdown() {
    for (const Client &client : this->clients_)
        client.socket->shutdown(SHUT_RDWR);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
void ModBusBridgeComponent::publish_sensor() {
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
void ModBusBridgeComponent::accept() 
{
    struct sockaddr_storage client_addr;
    socklen_t client_addrlen = sizeof(client_addr);
    std::unique_ptr<socket::Socket> socket = this->listener_->accept(reinterpret_cast<struct sockaddr *>(&client_addr), &client_addrlen);
    if (!socket)
        return;

    socket->setblocking(false);
    std::string identifier = socket->getpeername();
    this->clients_.emplace_back(std::move(socket), identifier);
    ESP_LOGI(TAG, "New client connected from %s", identifier.c_str());
    this->publish_sensor();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Cleanup closed connections
/////////////////////////////////////////////////////////////////////////////////////////////////////////////
void ModBusBridgeComponent::cleanup() 
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
    if (pos > 0)
        pos--; // Remove trailing colon
    buf[pos] = '\0'; // Null-terminate the string
    ESP_LOGD(tag, "%s %s", prefix, buf);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////
void ModBusBridgeComponent::read() 
{
    if (this->uart_->available() == 0)
        return;

    uint8_t b;
    // TODO: keep track of configured buffersize uart.buf_.size() < this->buf_size_
    while (this->uart_->available() >0 && this->uart_->read_byte(&b)) 
        uart_buf_.push_back(b);
    last_uart_usage_ = esphome::millis(); // register data comming in
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Exchange messages from socket to UART and back
/////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define MODBUS_RECEIVE_DELAY   100

void ModBusBridgeComponent::exchange() 
{
    uint8_t socket_buf[260]; // Buffer for reading socket data
    ssize_t socket_read_len;

    // First see if we have a client waiting for a reponse from the UART
    for (Client &client : this->clients_) 
    {
        if (client.disconnected)        // this client is disconneted -> skip
            continue;

        if (!client.uart_user_)         // this client is not waiting for a response -> skip
            continue;

        // found a client awaiting for UART response
        uint32_t time_delta = esphome::millis() - last_uart_usage_;

        // If we just send, or (still) receive data, wait for the UART to finish
        if (time_delta < MODBUS_RECEIVE_DELAY) 
            return; 

        // Validate the RTU frame in the UART
        int validation = this->validate_rtu_frame();
        
        // Below 0 we have a shortage of data
        if (validation < 0) 
        {
            if (time_delta > this->timeout_) {
                ESP_LOGW(TAG, "RTU response timeout for client %s", client.identifier.c_str());
                validation = 0x0B;   // exception code 6: Device is busy 0x0B is a better fit but triggers a new TCP connection 
            } else {
                // if we have nothing yet, we wait the full timeout for something to come in
                if (this->uart_buf_.size() == 0 || time_delta < (this->timeout_/10))  
                    return; 
                
                ESP_LOGE(TAG, "Incomplete RTU, short of %d bytes", validation *-1);
                validation = 0x06;   // exception code 6: Device is busy 0x0B is a better fit but triggers a new TCP connection 
            }
        }
        if (validation > 0)
        {
            LOG_BYTES(TAG, "RTU Frame <<<", this->uart_buf_.data(), this->uart_buf_.size());
            ESP_LOGW(TAG, "Send Exception code %d to TCP", validation);
            socket_buf[0] = this->last_unit_id_; 
            socket_buf[1] = this->last_function_code_ | 0x80; // set error flag
            socket_buf[2] = validation;  // exception code
            socket_buf[3] = 0x00;   // no CRC data
            socket_buf[4] = 0x00;   // no CRC data
            socket_read_len = 5;
        } else {
            // copy the data from the UART to the socket buffer
            socket_read_len = this->uart_buf_.size();
            memcpy(socket_buf, this->uart_buf_.data(), socket_read_len);
        }
        // validate and convert the UART data to Modbus TCP
        if (this->modbus_rtu_to_tcp(socket_buf, socket_read_len))
        {
            LOG_BYTES(TAG, "Send >>>", socket_buf, socket_read_len);
            int written = client.socket->write(socket_buf, socket_read_len);
            if (written != socket_read_len) {
                ESP_LOGE(TAG, "Failed to send all data to client %s, closing connection", client.identifier.c_str());
                client.disconnected = true;
                client.socket->close();
            }
        }
        // Clear the current client as we are done with the uart response
        client.uart_user_ = false;
        return;
    }

    // If we get here, then no client is waiting for an UART response,
    // so we can read from the socket to send new data to UART
    for (Client &client : this->clients_) 
    {
        if (client.disconnected)
            continue;

        socket_read_len = client.socket->read(socket_buf, sizeof(socket_buf));
        if (socket_read_len > 0) 
        {
//            LOG_BYTES(TAG, "Received <<<", socket_buf, socket_read_len);
            // Step 2: Send the data to the UART
            this->modbus_tcp_to_rtu(socket_buf, socket_read_len);
            this->uart_->flush();       // empty UART as we will write new data
            this->uart_buf_.clear();    // clear the buffer
            this->uart_->write_array(socket_buf, socket_read_len);

            // Mark the client as waiting for a UART response
            last_uart_usage_ = esphome::millis(); // Start the timeout timer
            client.uart_user_ = true;
            
            return; // we now wait for the UART response
        } 
        if (socket_read_len == 0 || errno == ECONNRESET) 
        {
            // Handle socket disconnection
            ESP_LOGI(TAG, "Client %s disconnected", client.identifier.c_str());
            client.disconnected = true;
            client.socket->close();
            continue;
        } 
        // socket_read_len < 0
        if (errno == EWOULDBLOCK || errno == EAGAIN) {  
            // No data available on the socket
            // cleanup clients which used the uart once, and have no communication for 60 seconds
//            if (esphome::millis() - client.last_uart_time > 60000) {
//                ESP_LOGD(TAG, "Client %s disconnected due to inactivity", client.identifier.c_str());
//                client.disconnected = true;
        } else {
            ESP_LOGW(TAG, "Failed to read from client %s with error %d!", client.identifier.c_str(), errno);
            client.disconnected = true;
        }
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
ModBusBridgeComponent::Client::Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier)
    : socket(std::move(socket)), identifier{identifier} 
{
    uart_user_ = false;
    disconnected = false;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
// RTU Frame validation, returns:
// <0 when frame to short, which can be used to wait some more time. the value indicates how many bytes are missing
// 0 when frame is valid
// >0 when frame is invalid, in which case the suggested exception code is returned (https://www.simplymodbus.ca/exceptions.htm)
/////////////////////////////////////////////////////////////////////////////////////////////////////////////
int ModBusBridgeComponent::validate_rtu_frame() 
{
    if (uart_buf_.size() < 4) // validate minimal length
        return -4;  

    // validate unit_id
    if (uart_buf_[0] != last_unit_id_) {
        ESP_LOGE(TAG, "Unit ID mismatch: %02X != %02X", uart_buf_[0], last_unit_id_);
        return 0x04;    // Slave device failure
    }
    // validate funciton_code: ignore error response as these are valid to communicate over TCP
    if ((uart_buf_[1] & 0x7F) != last_function_code_) {    
        ESP_LOGE(TAG, "Function code mismatch: %02X != %02X", this->uart_buf_[1], last_function_code_);
        return 0x04;    // Slave device failure
    }
    int frame_len = this->uart_buf_.size(); // unknown data length, set to what we have
    switch (this->uart_buf_[1]) 
    {
    case 0x01:  // Read Coils
    case 0x02:  // Read Discrete Inputs
    case 0x03:  // Read Holding Registers
    case 0x04:  // Read Input Registers
    case 0x0C:  // Get Comm Event Log
    case 0x11:  // Report Slave ID
    case 0x14:  // Read File Record
    case 0x17:  // Read/Write Multiple Registers
    case 0x18:  // Read FIFO Queue
    case 0x2B:  // Read Device Identification
        frame_len = this->uart_buf_[2] + 3 + 2; // data length + 3 bytes PDU header + 2 bytes CRC
        break;
    case 0x05:  // Write Single Coil
    case 0x06:  // Write Single Holding Register
    case 0x0B:  // Get Comm Event Counter
    case 0x0F:  // Write Multiple Coils
    case 0x10:  // Write Multiple Holding Registers
        frame_len = 4 + 2 + 2; // 4 bytes data + 2 bytes PDU header + 2 bytes CRC
        break;
    case 0x15:  // Write File Record
        frame_len = 2 + 2 + 2; // 2 bytes data + 2 bytes PDU header + 2 bytes CRC
        break;
    case 0x16:  // Mask Write Register
        frame_len = 6 + 2 + 2; // 4 bytes data + 2 bytes PDU header + 2 bytes CRC
        break;
    case 0x07:  // Read Exception Status
        frame_len = 1 + 2 + 2; // 1 bytes data + 2 bytes PDU header + 2 bytes CRC
        break;
    case 0x08:  // Diagnostics -> same as in request
    default:
        if (this->uart_buf_[1] & 0x80)
            frame_len = 1 + 2 + 2; // 1 bytes data + 2 bytes PDU header + 2 bytes CRC
        break;
    }
    if (uart_buf_.size() < frame_len) {
//        ESP_LOGW(TAG, "Frame length %d mismatch with expected %d", this->uart_buf_.size(), frame_len); // no logging as this will trigger a lot of messages
        return uart_buf_.size() - frame_len;    // return how many bytes are missing (negative value)
    }
    uint16_t crc = calculate_crc(this->uart_buf_.data(), frame_len - 2);
    uint16_t crc2 = this->uart_buf_[frame_len - 1] << 8 | this->uart_buf_[frame_len - 2];
    if (crc != crc2) {
        ESP_LOGE(TAG, "CRC mismatch: %04X != %04X", crc, crc2);
        return 0x05;    // Acknowledge
    }
    return 0;   // frame is valid
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Modbus RTU to TCP conversion logic -> Add MBAP header and strip CRC
/////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool ModBusBridgeComponent::modbus_rtu_to_tcp(uint8_t *frame, ssize_t &len) 
{
    if (len < 4) 
        return false;  
    uint16_t transaction_id = this->last_transaction_id_;
    uint16_t length = len-2; // the RTU frame without CRC
    memmove(frame +6, frame, length); // FIRST MOVE THE FRAME TO MAKE SPACE FOR THE MBAP HEADER
    frame[0] = (transaction_id >> 8) & 0xFF;
    frame[1] = transaction_id & 0xFF;
    frame[2] = 0x00;    // protocol_id = always 0x0000 for TCP Modbus
    frame[3] = 0x00;    // protocol_id = always 0x0000 for TCP Modbus
    frame[4] = (length >> 8) & 0xFF;
    frame[5] = length & 0xFF;
    len = 6 + length; // 6 bytes MBAP header + data length

    return true;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
/* Modbus TCP to RTU conversion logic
   Strip the MBAP header (first 7 bytes) and add CRC
Field	        Size    Description
Transaction ID	2	    Used to match the response to the request.
Protocol ID	    2	    Always 0x0000 for Modbus.
Length	        2	    Number of bytes in the remaining message (Unit ID + PDU).
*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool ModBusBridgeComponent::modbus_tcp_to_rtu(uint8_t *frame, ssize_t &len) 
{
    if (len < 8) {
        ESP_LOGE(TAG, "TCP Frame too short for conversion to RTU");
        return false;
    }
    this->last_transaction_id_ = (frame[0] << 8) | frame[1];
    ssize_t frame_len = (frame[4] << 8) | frame[5];
    if (len < frame_len + 6) {  // we allow for longer frames, but not shorter
        ESP_LOGE(TAG, "Invalid Modbus TCP frame length");
        return false;
    }
    len = frame_len; // remove unit_id
    memmove(frame, frame +6, len); // Shift TCP frame to remove MBAP header

    uint16_t crc = calculate_crc(frame, len);
    frame[len++] = crc & 0xFF;
    frame[len++] = (crc >> 8) & 0xFF;

    last_unit_id_ = frame[0];           // store for response validation
    last_function_code_ = frame[1];     // store for response validation

    return true;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint16_t ModBusBridgeComponent::calculate_crc(const uint8_t *data, size_t len) {
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

/*
Code	Description	                    Byte Count	Notes
0x01	Read Coils	                    ✅ Yes	Returns multiple coil (bit) values.
0x02	Read Discrete Inputs	        ✅ Yes	Returns multiple discrete input (bit) values.
0x03	Read Holding Registers	        ✅ Yes	Returns multiple holding register values.
0x04	Read Input Registers	        ✅ Yes	Returns multiple input register values.
0x05	Write Single Coil	            ❌ No	Response echoes request (fixed length).
0x06	Write Single Holding Register	❌ No	Response echoes request (fixed length).
0x07	Read Exception Status	        ❌ No	Returns a single byte (status of 8 coils).
0x08	Diagnostics	                    ❌ No	Used for communication tests (echo request).
0x0B	Get Comm Event Counter	        ❌ No	Returns 2 bytes (event counter).
0x0C	Get Comm Event Log	            ✅ Yes	Returns a log of events with byte count.
0x0F	Write Multiple Coils	        ❌ No	Response echoes request (fixed length).
0x10	Write Multiple Holding Registers❌ No	Response echoes request (fixed length).
0x11	Report Slave ID	                ✅ Yes	Returns device-specific data.
0x14	Read File Record	            ✅ Yes	Returns data from a file record.
0x15	Write File Record	            ❌ No	Response is fixed-length acknowledgment.
0x16	Mask Write Register	            ❌ No	Used to modify specific bits in a register.
0x17	Read/Write Multiple Registers	✅ Yes	Reads & writes multiple registers in one request.
0x18	Read FIFO Queue	                ✅ Yes	Returns queued data from the slave.
0x2B	Read Device Identification	    ✅ Yes	Returns device identification info.
*/