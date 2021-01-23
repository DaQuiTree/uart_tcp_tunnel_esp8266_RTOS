# uart_tcp_tunnel_esp8266_RTOS
基于esp8266 RTOS，安信可 ESP-01的uart->tcp tunnel，可用于将串口数据转换成无线输出调试。

## 功能
1、8266模块是tcp client做收发，可通过串口修改server ip/port，协议格式为

IP: SipS192.168.1.100S 

PORT: SportS1234S

2、可通过ESPTouch + smartconfig做wifi配网

