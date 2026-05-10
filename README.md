Modbus TCP to RTU Bridge for ESPHome
=========================

Custom component for ESPHome to bridge a ModBus (RS485) UART stream over WiFi or Ethernet. Provides a serial-to-wifi bridge using ESPHome.

This component creates a TCP server listening on port 502 (by default), and relays all data between the connected
clients and the serial port.

Usage
-----

Requires ESPHome v2022.3.0 or newer.

```yaml
external_components:
  - source: github://Pluimvee/esphome-modbus-tcp-to-rtu

modbus_bridge:
```

You can set the UART ID and port to be used under the `modbus_bridge` component.

```yaml
uart:
   id: modbus
   # add further configuration for the UART here

modbus_bridge:
   uart_id: modbus
```

Sensors
-------
The server provides a binary sensor that signals whether there currently is a client connected:

```yaml
binary_sensor:
  - platform: modbus_bridge
    connected:
      name: Connected
```

It also provides a numeric sensor that indicates the number of connected clients:

```yaml
sensor:
  - platform: modbus_bridge
    connection_count:
      name: "Number of connections"
```

Advanced
--------
In case you are using the same UART as used for logging you need to disable the logger for using the serial by setting the baud_rate to 0:

```yaml
logger:
  level: DEBUG  
  baud_rate: 0  # Disable using Serial for logging

uart:
  id: modbus
  tx_pin: GPIO1
  rx_pin: GPIO3
  baud_rate: 9600
  rx_buffer_size: 512
  debug:        # Enable UART logging

modbus_bridge:
   uart_id: modbus
   port: 502
   buffer_size: 512
   timeout: 3000
```

The stream server has an internal buffer into which UART data is read before it is transmitted over TCP. The size of
this buffer can be changed using the `buffer_size` option, and must be a power of two. Increasing the buffer size above
the default of 256 bytes can help to achieve optimal throughput, and is especially helpful when using high baudrates. It
can also be necessary to increase the [`rx_buffer_size`][uart-config] option of the UART itself.

[uart-config]: https://esphome.io/components/uart.html#configuration-variables

Hardware used
--------
- ESP 12F (1.10 EUR)
- MAX485 Module (1.30)
- AC 220v to DC 5v (2.20 EUR)

<img width="485" height="391" alt="image" src="https://github.com/user-attachments/assets/a29901b8-1a84-4165-9809-11f85063ef39" />

