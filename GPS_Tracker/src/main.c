#include <stdio.h>
#include <ncs_version.h>
//Kernel, socket and CoAP APIs
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/coap.h>
//Logging, buttons and LEDs, modem and LTE link control
#include <zephyr/logging/log.h>
#include <dk_buttons_and_leds.h>
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
//For token generation
#include <zephyr/random/random.h>
//For modem key management and TLS credentials
#include <modem/modem_key_mgmt.h>
#include <zephyr/net/tls_credentials.h>
//GNSS API for getting location data from the modem
#include <nrf_modem_gnss.h>
#define SEC_TAG 12

LOG_MODULE_REGISTER(Tracker, LOG_LEVEL_INF);

//LTE
K_SEM_DEFINE(lte_connected, 0, 1); //semaphore to wait for LTE connection
static void lte_handler(const struct lte_lc_evt *const evt){
        switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
			(evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			break;
		}
		LOG_INF("Network registration status: %s",
				evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
				"Connected - home network" : "Connected - roaming");
                        k_sem_give(&lte_connected);
		break;
	case LTE_LC_EVT_RRC_UPDATE:
		LOG_INF("RRC mode: %s", evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ?
				"Connected" : "Idle");
		break;

        case LTE_LC_EVT_PSM_UPDATE:
	LOG_INF("PSM parameter update: TAU: %d, Active time: %d",
		evt->psm_cfg.tau, evt->psm_cfg.active_time);
	if (evt->psm_cfg.active_time == -1){
		LOG_ERR("Network rejected PSM parameters. Failed to enable PSM");
	}
	break;

        case LTE_LC_EVT_EDRX_UPDATE:
	LOG_INF("eDRX parameter update: eDRX: %f, PTW: %f",
		(double)evt->edrx_cfg.edrx, (double)evt->edrx_cfg.ptw);
	break;
	default:
		break;
	}
}

//MODEM
static int modem_configure(void)
{
        int err;

        LOG_INF("Initializing modem library");
        //Initialize the modem library
        err = nrf_modem_lib_init();
        if (err) {
                LOG_ERR("Failed to initialize the modem library, error: %d", err);
                return err;
        }

        //Write the seciurity credentials to the modem
        err = modem_key_mgmt_write(SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_IDENTITY, CONFIG_COAP_DEVICE_NAME, 
			strlen(CONFIG_COAP_DEVICE_NAME));
        if (err) {
	        LOG_ERR("Failed to write identity: %d\n", err);
	        return err;
        }

        err = modem_key_mgmt_write(SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_PSK, CONFIG_COAP_SERVER_PSK,
                        strlen(CONFIG_COAP_SERVER_PSK));
        if (err) {
	        LOG_ERR("Failed to write PSK: %d\n", err);
	        return err;
        }

        //Request PSM and eDRX from the network
        err = lte_lc_psm_req(true);
	if (err) {
		LOG_ERR("lte_lc_psm_req, error: %d", err);
	} 
	err = lte_lc_edrx_req(true);
	if (err) {
	        LOG_ERR("lte_lc_edrx_req, error: %d", err);
	}

        LOG_INF("Connecting to LTE network");
        err = lte_lc_connect_async(lte_handler);
        if (err) {
                LOG_ERR("Error in lte_lc_connect_async, error: %d", err);
                return err;
        }
        k_sem_take(&lte_connected, K_FOREVER);
        return 0;
}

//SOCKET
static int sock;
static struct sockaddr_storage server;
static int server_resolve(void){
        int err;
        struct addrinfo *result;
        struct addrinfo hints = {
	        .ai_family = AF_INET,
	        .ai_socktype = SOCK_DGRAM
        };
	
        err = getaddrinfo(CONFIG_COAP_SERVER_HOSTNAME, NULL, &hints, &result);
        if (err != 0) {
	        LOG_INF("ERROR: getaddrinfo failed %d\n", err);
	        return -EIO;
        }

        if (result == NULL) {
	        LOG_INF("ERROR: Address not found\n");
	return -ENOENT;
        }

        struct sockaddr_in * server_info = ((struct sockaddr_in *)&server);

        server_info->sin_addr.s_addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
        server_info->sin_family = AF_INET;
        server_info->sin_port = htons(CONFIG_COAP_SERVER_PORT);

        //Convert the IP address to a string and log it
        char ipv4_addr[NET_IPV4_ADDR_LEN];
        inet_ntop(AF_INET, &server_info->sin_addr.s_addr, ipv4_addr,
        sizeof(ipv4_addr));
        LOG_INF("IPv4 Address found %s", ipv4_addr);

        freeaddrinfo(result);

        return 0;
}  
static int server_connect(void){
        int err;
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_DTLS_1_2);
        if (sock < 0) {
                LOG_INF("Failed to create socket: %d.\n", errno);
        return -errno;

        enum {
	NONE = 0,
	OPTIONAL = 1,
	REQUIRED = 2,
	};

	int verify = REQUIRED;
        err = setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify));
	if (err) {
		LOG_ERR("Failed to setup peer verification, errno %d\n", errno);
		return -errno;
	}

        }

        err = setsockopt(sock, SOL_TLS, TLS_HOSTNAME, CONFIG_COAP_SERVER_HOSTNAME,
	 strlen(CONFIG_COAP_SERVER_HOSTNAME));
	if (err) {
		LOG_ERR("Failed to setup TLS hostname (%s), errno %d\n",
			CONFIG_COAP_SERVER_HOSTNAME, errno);
		return -errno;
	}

        sec_tag_t sec_tag_list[] = { SEC_TAG };

        err = setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, sec_tag_list,
		 sizeof(sec_tag_t) * ARRAY_SIZE(sec_tag_list));
        if (err) {
	        LOG_ERR("Failed to setup socket security tag, errno %d\n", errno);
	        return -errno;
        }

        err = connect(sock, (struct sockaddr *)&server, sizeof(struct sockaddr_in));
        if (err < 0) {
                LOG_INF("Connect failed : %d\n", errno);
        return -errno;
        }
        LOG_INF("Successfully connected to server");
        return 0;
}

//COAP
#define APP_COAP_VERSION 1
#define APP_COAP_MAX_MSG_LEN 1280
#define MESSAGE_TO_SEND "Hello from Zephyr!"
static uint8_t coap_buf[APP_COAP_MAX_MSG_LEN];
static uint16_t next_token;
static int client_get_send(void){
        int err;
        struct coap_packet request;

        next_token = sys_rand32_get();

        err = coap_packet_init(&request, coap_buf, sizeof(coap_buf),
        APP_COAP_VERSION, COAP_TYPE_NON_CON,
        sizeof(next_token), (uint8_t *)&next_token,
        COAP_METHOD_GET, coap_next_id());
        if (err < 0) {
                LOG_INF("Failed to create CoAP request, %d\n", err);
        return err;
        }

        err = coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
        (uint8_t *)CONFIG_COAP_RX_RESOURCE,
        strlen(CONFIG_COAP_RX_RESOURCE));
        if (err < 0) {
                LOG_INF("Failed to encode CoAP option, %d\n", err);
        return err;
        }

        err = send(sock, request.data, request.offset, 0);
        if (err < 0) {
                LOG_INF("Failed to send CoAP request, %d\n", errno);
        return -errno;
        }

        LOG_INF("CoAP GET request sent: Token 0x%04x\n", next_token);

        return 0;
} 
static int client_put_send(uint8_t *message, uint16_t message_len){
        int err;
        struct coap_packet request;

        next_token = sys_rand32_get();

        err = coap_packet_init(&request, coap_buf, sizeof(coap_buf),
        APP_COAP_VERSION, COAP_TYPE_NON_CON,
        sizeof(next_token), (uint8_t *)&next_token,
        COAP_METHOD_PUT, coap_next_id());
        if (err < 0) {
                LOG_INF("Failed to create CoAP request, %d\n", err);
        return err;
        }

        err = coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
        (uint8_t *)CONFIG_COAP_TX_RESOURCE,
        strlen(CONFIG_COAP_TX_RESOURCE));
        if (err < 0) {
                LOG_INF("Failed to encode CoAP option, %d\n", err);
        return err;
        }

        const uint8_t text_plain = COAP_CONTENT_FORMAT_TEXT_PLAIN;
        err = coap_packet_append_option(&request, COAP_OPTION_CONTENT_FORMAT,
                                        &text_plain,
                                        sizeof(text_plain));
        if (err < 0) {
                LOG_INF("Failed to encode CoAP option, %d\n", err);
        return err;
        }

        err = coap_packet_append_payload_marker(&request);
        if (err < 0) {
                LOG_INF("Failed to append payload marker, %d\n", err);
        return err;
        }

        err = coap_packet_append_payload(&request, (uint8_t *)message, message_len);
        if (err < 0) {
                LOG_INF("Failed to append payload, %d\n", err);
        return err;
        }

        err = send(sock, request.data, request.offset, 0);
        if (err < 0) {
                LOG_INF("Failed to send CoAP request, %d\n", errno);
        return -errno;
        }

        LOG_INF("CoAP PUT request sent: Token 0x%04x\n", next_token);
        return 0;

}
static int client_handle_response(uint8_t *buf, int received){
        int err;
        struct coap_packet reply;
        const uint8_t *payload;
        uint16_t payload_len;
        uint8_t token[8];
        uint16_t token_len;
        uint8_t temp_buf[128];

        err = coap_packet_parse(&reply, buf, received, NULL, 0);
        if (err < 0) {
                LOG_INF("Malformed response received: %d\n", err);
        return err;
        }

        payload = coap_packet_get_payload(&reply, &payload_len);
        token_len = coap_header_get_token(&reply, token);

        if ((token_len != sizeof(next_token)) ||
            (memcmp(&next_token, token, sizeof(next_token)) != 0)) {
                LOG_INF("Invalid token received: 0x%02x%02x\n",
                token[1], token[0]);
        return 0;
        }

        if (payload_len > 0) {
                snprintf(temp_buf, MIN(payload_len + 1, sizeof(temp_buf)), "%s", payload);
        } else {
                strcpy(temp_buf, "EMPTY");
        }

        LOG_INF("CoAP response: Code 0x%x, Token 0x%02x%02x, Payload: %s\n",
               coap_header_get_code(&reply), token[1], token[0], (char *)temp_buf);

        return 0;

}

//Keep server conection alive
#define KEEP_ALIVE_INTERVAL 30000 //30 seconds

//GNSS
static int64_t gnss_start_time;
static bool first_fix = false;
static uint8_t gps_data[256] = {0}; //Buffer to hold the GPS data to be sent to the server
static struct nrf_modem_gnss_pvt_data_frame pvt_data;
K_SEM_DEFINE(gnss_fix_sem, 0, 1); //semaphore to wait for GNSS fix

static void print_fix_data(struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
	LOG_INF("Latitude:       %.06f", pvt_data->latitude);
	LOG_INF("Longitude:      %.06f", pvt_data->longitude);
	LOG_INF("Altitude:       %.01f m", (double)pvt_data->altitude);
	LOG_INF("Time (UTC):     %02u:%02u:%02u.%03u",
	       pvt_data->datetime.hour,
	       pvt_data->datetime.minute,
	       pvt_data->datetime.seconds,
	       pvt_data->datetime.ms);
        LOG_INF("GNSS Accuracy:     %.01f m", (double)pvt_data->accuracy);
        LOG_INF("Execution time:     %d ms", pvt_data->execution_time);
        int err = snprintf(gps_data, sizeof(gps_data), "Latitude: %.06f, Longitude: %.06f", pvt_data->latitude, pvt_data->longitude);		
	if (err < 0) {
		LOG_ERR("Failed to print to buffer: %d", err);
	}
}
static void gnss_event_handler(int event)
{
	int err, num_satellites;

	switch (event) {
	case NRF_MODEM_GNSS_EVT_PVT:
		num_satellites = 0;
		for (int i = 0; i < 12 ; i++) {
			if (pvt_data.sv[i].signal != 0) {
				num_satellites++;
			}
		}
		LOG_INF("Searching. Current satellites: %d", num_satellites);
		err = nrf_modem_gnss_read(&pvt_data, sizeof(pvt_data), NRF_MODEM_GNSS_DATA_PVT);
		if (err) {
			LOG_ERR("nrf_modem_gnss_read failed, err %d", err);
			return;
		}
		if (pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
			dk_set_led_on(DK_LED1);
			print_fix_data(&pvt_data);
			if (!first_fix) {
				LOG_INF("Time to first fix: %2.1lld s", (k_uptime_get() - gnss_start_time)/1000);
				first_fix = true;
			}
			return;
		}
		//Check for the flags indicating GNSS is blocked
		if (pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_DEADLINE_MISSED) {
		LOG_INF("GNSS blocked by LTE activity");
		} if (pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME) {
		LOG_INF("Insufficient GNSS time window");
		}

		break;

	case NRF_MODEM_GNSS_EVT_PERIODIC_WAKEUP:
		LOG_INF("GNSS has woken up");
		break;
	case NRF_MODEM_GNSS_EVT_SLEEP_AFTER_FIX:
		LOG_INF("GNSS enter sleep after fix");
                k_sem_give(&gnss_fix_sem);
		break;
	default:
		break;
	}
}
static int gnss_init_and_start(void)
{

	/* STEP 4 - Set the modem mode to normal */
	if (lte_lc_func_mode_set(LTE_LC_FUNC_MODE_NORMAL) != 0) {
	LOG_ERR("Failed to activate GNSS functional mode");
	return -1;
	}

	if (nrf_modem_gnss_event_handler_set(gnss_event_handler) != 0) {
		LOG_ERR("Failed to set GNSS event handler");
		return -1;
	}

	if (nrf_modem_gnss_fix_interval_set(CONFIG_GNSS_PERIODIC_INTERVAL) != 0) {
		LOG_ERR("Failed to set GNSS fix interval");
		return -1;
	}

	if (nrf_modem_gnss_fix_retry_set(CONFIG_GNSS_PERIODIC_TIMEOUT) != 0) {
		LOG_ERR("Failed to set GNSS fix retry");
		return -1;
	}

	LOG_INF("Starting GNSS");
	if (nrf_modem_gnss_start() != 0) {
		LOG_ERR("Failed to start GNSS");
		return -1;
	}

	gnss_start_time = k_uptime_get();

	return 0;
}

static void button_handler(uint32_t button_state, uint32_t has_changed)
{

	if (has_changed & DK_BTN1_MSK && button_state & DK_BTN1_MSK) {
		client_get_send();
	} else if (has_changed & DK_BTN2_MSK && button_state & DK_BTN2_MSK) {
		client_put_send(&gps_data, sizeof(gps_data));
	}
}

int main(void)
{
        int err;
        int recieved;
        //Establish LTE connection
        err = modem_configure();
        if (err) {
	        LOG_ERR("Failed to configure the modem");
	        return 0;
        }
        //Find the server's IP address
        if (server_resolve() != 0) {
		LOG_INF("Failed to resolve server name");
		return 0;
	}
        //Connect to the server
	if (server_connect() != 0) {
		LOG_INF("Failed to initialize client");
		return 0;
	}

        if(client_put_send(MESSAGE_TO_SEND, sizeof(MESSAGE_TO_SEND)) != 0){
                LOG_INF("Failed to send CoAP PUT request");
                return 0;
        }

        if (dk_buttons_init(button_handler) != 0) {
		LOG_ERR("Failed to initialize the buttons library");
	}

        if (gnss_init_and_start() != 0) {
		LOG_ERR("Failed to initialize and start GNSS");
		return 0;
	}

        while(1) {
                k_sem_take(&gnss_fix_sem, K_FOREVER);

                if(k_uptime_get() - gnss_start_time > KEEP_ALIVE_INTERVAL){
                        LOG_INF("Reconecting to server");
                        if (server_connect() != 0) {
                                LOG_ERR("Failed to initialize client");
                                break;
                        }
                        gnss_start_time = k_uptime_get();
                }

                client_put_send(&gps_data, sizeof(gps_data));

                recieved = recv(sock, coap_buf, sizeof(coap_buf), 0);

		if (recieved < 0) {
			LOG_ERR("Socket error: %d, exit\n", errno);
			break;
		} else if (recieved == 0) {
			LOG_INF("Empty datagram\n");
			continue;
		}

		err = client_handle_response(coap_buf, recieved);
		if (err < 0) {
			LOG_ERR("Invalid response, exit\n");
			break;
		}
        }
        (void)close(sock);
        return 0;
}