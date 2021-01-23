#ifndef _TUNNEL_SMARTCONFIG_H_
#define _TUNNEL_SMARTCONFIG_H_

int smartconfig_get_wifi_connect_state(void);

int smartconfig_check_nvs(void);

void smartconfig_normal_wifi_connect(void);

void smartconfig_mode_start(void);

int smartconfig_nvs_get_serverip(char *ip, int len);

uint16_t smartconfig_nvs_get_serverport(void);

int smartconfig_nvs_set_serverinfo(char *ip, uint16_t port);

//tunnel 
void tunnel_set_flashing_delay(int delay_ms);

#endif