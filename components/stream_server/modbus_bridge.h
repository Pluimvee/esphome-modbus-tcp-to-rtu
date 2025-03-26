#pragma once

#include "esphome/core/component.h"
#include "esphome/components/socket/socket.h"
#include "esphome/components/uart/uart.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif

#include <memory>
#include <string>
#include <vector>

class ModBusBridgeComponent : public esphome::Component {
public:
    ModBusBridgeComponent() = default;
    explicit ModBusBridgeComponent(esphome::uart::UARTComponent *stream) : uart_{stream} {}
    void set_uart_parent(esphome::uart::UARTComponent *parent) { this->uart_ = parent; }
    void set_buffer_size(size_t size) { this->buf_size_ = size; }
    void set_port(uint16_t port) { this->port_ = port; }
    void set_timeout(uint16_t timeout) { this->timeout_ = timeout; }

#ifdef USE_BINARY_SENSOR
    void set_connected_sensor(esphome::binary_sensor::BinarySensor *connected) { this->connected_sensor_ = connected; }
#endif
#ifdef USE_SENSOR
    void set_connection_count_sensor(esphome::sensor::Sensor *connection_count) { this->connection_count_sensor_ = connection_count; }
#endif

    void setup() override;
    void loop() override;
    void dump_config() override;
    void on_shutdown() override;

    float get_setup_priority() const override { return esphome::setup_priority::AFTER_WIFI; }

protected:
    void publish_sensor();

    void accept();      // accept new connections
    void read();        // read data from uart
    void exchange();    // exchange data between clients and uart
    void cleanup();     // cleanup disconnected clients 

    int validate_rtu_frame();   // validates UART contents for valid RTU response

    // some helpers
    bool modbus_tcp_to_rtu(uint8_t *frame, ssize_t &len);
    bool modbus_rtu_to_tcp(uint8_t *frame, ssize_t &len);
    uint16_t calculate_crc(const uint8_t *data, size_t len);

    struct Client {
        Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier);

        std::unique_ptr<esphome::socket::Socket> socket{nullptr};
        std::string identifier{};
        bool disconnected{false};
        bool uart_user_{false};        // Track if UART is in use by this client
    };

    // Component management
    uint16_t port_{502};
    std::unique_ptr<esphome::socket::Socket> listener_{};   // hte accepting socket
    std::vector<Client> clients_{};                         // list of connected clients

    // UART management
    esphome::uart::UARTComponent *uart_{nullptr};
    size_t buf_size_{300};
    std::vector<uint8_t> uart_buf_;     // Buffer for UART response
    uint32_t last_uart_usage_{0};       // Track the last time the UART was used

    // ModBus TCP management (MBAP header)
    uint16_t last_transaction_id_{0};
    uint16_t timeout_{3000};

    // ModBus RTU management (PDU header)
    uint8_t last_unit_id_{0};
    uint8_t last_function_code_{0};

#ifdef USE_BINARY_SENSOR
    esphome::binary_sensor::BinarySensor *connected_sensor_;
#endif
#ifdef USE_SENSOR
    esphome::sensor::Sensor *connection_count_sensor_;
#endif
};
