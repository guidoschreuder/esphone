
#include "ebus_component.h"

namespace esphome {
namespace ebus {

  static const char *const TAG = "ebus";


void EbusComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "EbusComponent");
  ESP_LOGCONFIG(TAG, "  master_addres: %02X", this->master_address_);
}

void EbusComponent::setup() {
  setup_queues();
  setup_ebus();
  setup_uart();
  setup_tasks();
}

void EbusComponent::set_master_address(uint8_t master_address) {
  this->master_address_ = master_address;
}
void EbusComponent::set_max_tries(uint8_t max_tries) {
  this->max_tries_ = max_tries;
}
void EbusComponent::set_max_lock_counter(uint8_t max_lock_counter) {
  this->max_lock_counter_ = max_lock_counter;
}

void EbusComponent::set_uart_num(uint8_t uart_num) {
  this->uart_num_ = uart_num;
}
void EbusComponent::set_uart_tx_pin(uint8_t uart_tx_pin) {
  this->uart_tx_pin_ = uart_tx_pin;
}
void EbusComponent::set_uart_rx_pin(uint8_t uart_rx_pin) {
  this->uart_rx_pin_ = uart_rx_pin;
}
void EbusComponent::set_history_queue_size(uint8_t history_queue_size) {
  this->history_queue_size_ = history_queue_size;
}
void EbusComponent::set_command_queue_size(uint8_t command_queue_size) {
  this->command_queue_size_ = command_queue_size;
}

void EbusComponent::setup_queues() {
  history_queue_ = xQueueCreate(this->history_queue_size_, sizeof(Ebus::Telegram));
  command_queue_ = xQueueCreate(this->command_queue_size_, sizeof(Ebus::Telegram));
}
void EbusComponent::setup_ebus() {
  Ebus::ebus_config_t ebus_config = Ebus::ebus_config_t {
    .master_address = this->master_address_,
    .max_tries = this->max_tries_,
    .max_lock_counter = this->max_lock_counter_,
  };
  ebus = new Ebus::Ebus(ebus_config);

  ebus->set_uart_send_function( [&](const char * buffer, int16_t length) { return uart_write_bytes(this->uart_num_, buffer, length); } );

  ebus->set_queue_received_telegram_function( [&](Ebus::Telegram &telegram) {
    BaseType_t xHigherPriorityTaskWoken;
    xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendToBackFromISR(this->history_queue_, &telegram, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
      portYIELD_FROM_ISR();
    }
  } );

  ebus->set_deueue_command_function( [&](void *const command) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (xQueueReceiveFromISR(this->command_queue_, command, &xHigherPriorityTaskWoken)) {
      if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
      }
      return true;
    }
    return false;
  } );

}

void EbusComponent::setup_uart() {
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&mux);

  uart_config_t uart_config = {
    .baud_rate = 2400,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .rx_flow_ctrl_thresh = 2,
    .use_ref_tick = true,
  };

  ESP_ERROR_CHECK(uart_param_config(this->uart_num_, &uart_config));

  ESP_ERROR_CHECK(uart_set_pin(
      this->uart_num_,
      this->uart_tx_pin_,
      this->uart_rx_pin_,
      UART_PIN_NO_CHANGE,
      UART_PIN_NO_CHANGE));

  ESP_ERROR_CHECK(uart_driver_install(this->uart_num_, 256, 0, 0, NULL, 0));

  portEXIT_CRITICAL(&mux);
}

void EbusComponent::setup_tasks() {
  xTaskCreate(&process_received_bytes, "ebus_process_received_bytes", 2048, (void*) this, 10, NULL);
  xTaskCreate(&process_received_messages, "ebus_process_received_messages", 2560, (void*) this, 5, NULL);
}

void EbusComponent::process_received_bytes(void *pvParameter) {
  EbusComponent* instance = static_cast<EbusComponent*>(pvParameter);

  while (1) {
    uint8_t receivedByte;
    int len = uart_read_bytes(instance->uart_num_, &receivedByte, 1, 20 / portTICK_PERIOD_MS);
    if (len) {
      instance->ebus->process_received_char(receivedByte);
      taskYIELD();
    }
  }
}

void EbusComponent::process_received_messages(void *pvParameter) {
  EbusComponent* instance = static_cast<EbusComponent*>(pvParameter);

  Ebus::Telegram telegram;
  while (1) {
    if (xQueueReceive(instance->history_queue_, &telegram, pdMS_TO_TICKS(1000))) {
      instance->handle_message(telegram);
      // TODO: this comment is kept as reference on how to debug stack overflows. Could be generalized.
      // ESP_LOGD(TAG, "Task: %s, Stack Highwater Mark: %d", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL));
      taskYIELD();
    }
  }
}

void EbusComponent::handle_message(Ebus::Telegram &telegram) {
  if (telegram.getState() != Ebus::TelegramState::endCompleted) {
    ESP_LOGD(TAG, "Message received with invalid state: %s, QQ:%02X, ZZ:%02X, Command:%02X%02X",
             telegram.getStateString(),
             telegram.getQQ(),
             telegram.getZZ(),
             telegram.getPB(),
             telegram.getSB());
    return;
  }

  for (auto const& message_handler : message_handlers) {
    message_handler(telegram);
  }
}

} // namespace ebus
} // namespace esphome