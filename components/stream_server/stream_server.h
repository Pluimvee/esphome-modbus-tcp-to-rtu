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
#include <mutex>

class StreamServerComponent : public esphome::Component {
public:
    StreamServerComponent() = default;
    explicit StreamServerComponent(esphome::uart::UARTComponent *stream) : stream_{stream} {}
    void set_uart_parent(esphome::uart::UARTComponent *parent) { this->stream_ = parent; }
    void set_buffer_size(size_t size) { this->buf_size_ = size; }

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

    void set_port(uint16_t port) { this->port_ = port; }
    void set_modbus(bool modbus) { this->modbus_ = modbus; }

protected:
    void publish_sensor();

    void accept();      // accept new connections
    void exchange();    // exchange data between clients and uart
    void cleanup();     // cleanup disconnected clients 

    void modbus_tcp_to_rtu(uint8_t *frame, ssize_t &len);
    void modbus_rtu_to_tcp(uint8_t *frame, ssize_t &len);
    uint16_t calculate_crc(const uint8_t *data, size_t len);

    struct Client {
        Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier);

        std::unique_ptr<esphome::socket::Socket> socket{nullptr};
        std::string identifier{};
        bool disconnected{false};
        uint32_t last_uart_time{0};    // Track the start time for UART response
    };

    esphome::uart::UARTComponent *stream_{nullptr};
    uint16_t port_;
    size_t buf_size_;
    bool modbus_{true};
    Client *current_client_{nullptr}; // Track the client currently using the UART

#ifdef USE_BINARY_SENSOR
    esphome::binary_sensor::BinarySensor *connected_sensor_;
#endif
#ifdef USE_SENSOR
    esphome::sensor::Sensor *connection_count_sensor_;
#endif

    std::unique_ptr<esphome::socket::Socket> socket_{};
    std::vector<Client> clients_{};
};
