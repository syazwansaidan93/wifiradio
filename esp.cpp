#include <WiFi.h>
#include <esp_wifi.h>
#include <driver/i2s.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_sleep.h>
#include <Wire.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <arduinoFFT.h>
#include <math.h>

const char* ssid = "wifi_slow2";
const char* orangePiIP = "192.168.1.3";
const uint16_t tcpPort = 8888;
const uint16_t httpPort = 8889;

WiFiClient client;

#define I2S_NUM I2S_NUM_0
#define I2S_BCK_IO 1
#define I2S_WS_IO 2
#define I2S_DOUT_IO 3

const size_t AUDIO_BUFFER_SIZE = 65536;
const size_t MINIMUM_BUFFER_SIZE = 32768;
uint8_t audioBuffer[AUDIO_BUFFER_SIZE];

volatile size_t bufferHead = 0;
volatile size_t bufferTail = 0;
volatile size_t bytesInCircularBuffer = 0;

portMUX_TYPE bufferMutex = portMUX_INITIALIZER_UNLOCKED;

const gpio_num_t GPIO_SENSOR_PIN = GPIO_NUM_4;
const gpio_num_t GPIO_STATE_IND_PIN = GPIO_NUM_8;
const gpio_num_t GPIO_STREAM_IND_PIN = GPIO_NUM_5;

TaskHandle_t readTaskHandle = NULL;
TaskHandle_t writeTaskHandle = NULL;
TaskHandle_t displayTaskHandle = NULL;

volatile bool isDisplayActive = true;

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_SDA GPIO_NUM_6
#define OLED_SCL GPIO_NUM_7
#define OLED_RESET -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

struct NowPlaying {
	String artist = "Loading...";
	String title = "Initializing...";
};
NowPlaying currentSong;

static int16_t artistScrollPos = 0;
static int16_t titleScrollPos = 0;
static unsigned long lastArtistScrollTime = 0;
static unsigned long lastTitleScrollTime = 0;
static unsigned long artistScrollCompleteTime = 0;
static unsigned long titleScrollCompleteTime = 0;
static unsigned long songStartTime = 0;

const uint16_t SCROLL_STEP_DELAY_MS = 10;
const uint16_t SCROLL_RESET_DELAY_MS = 2000;
const uint16_t SCROLL_START_DELAY_MS = 2000;
const uint8_t ARTIST_MAX_PX = 90;

static unsigned long lastEQUpdate = 0;
static uint8_t eqLevels[30] = {0};

#define FFT_N 256
double vReal[FFT_N];
double vImag[FFT_N];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, FFT_N, 44100);

const int NUM_BARS = 30;
const double MAX_MAG = FFT_N / 2.0;

void drawWiFiBar(int x, int y, int rssi) {
	int bars = 0;
	if (rssi > -50) bars = 4;
	else if (rssi > -60) bars = 3;
	else if (rssi > -70) bars = 2;
	else if (rssi > -80) bars = 1;

	int w = 3;
	int h0 = 4;
	int spacing = 1;

	for (int i = 0; i < 4; i++) {
		int bar_h = h0 + (i * 2);
		int bar_x = x + (i * (w + spacing));
		int bar_y = y - bar_h;
		if (i < bars) {
			display.fillRect(bar_x, bar_y, w, bar_h, SSD1306_WHITE);
		} else {
			display.drawRect(bar_x, bar_y, w, bar_h, SSD1306_WHITE);
		}
	}
}

void updateEQGraphic() {
	unsigned long now = millis();
	if (now - lastEQUpdate < 50) return;
	lastEQUpdate = now;

	size_t neededBytes = FFT_N * 4;
	portENTER_CRITICAL(&bufferMutex);
	if (bytesInCircularBuffer < neededBytes) {
		portEXIT_CRITICAL(&bufferMutex);
		return;
	}

	size_t copyPos = bufferTail;
	for (int i = 0; i < FFT_N; i++) {
		int16_t left = (int16_t)(audioBuffer[copyPos] | (audioBuffer[(copyPos + 1) % AUDIO_BUFFER_SIZE] << 8));
		int16_t right = (int16_t)(audioBuffer[(copyPos + 2) % AUDIO_BUFFER_SIZE] | (audioBuffer[(copyPos + 3) % AUDIO_BUFFER_SIZE] << 8));
		double avgSample = ((double)left + (double)right) / 2.0 / 32768.0;
		vReal[i] = avgSample;
		vImag[i] = 0.0;
		copyPos = (copyPos + 4) % AUDIO_BUFFER_SIZE;
	}
	portEXIT_CRITICAL(&bufferMutex);

	FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
	FFT.compute(FFTDirection::Forward);
	FFT.complexToMagnitude();

	int binsPerBand = 128 / NUM_BARS;
	for (int b = 0; b < NUM_BARS; b++) {
		int start = 1 + b * binsPerBand;
		int end = start + binsPerBand - 1;
		if (end > 127) end = 127;

		double maxMag = 0.0;
		for (int k = start; k <= end; k++) {
			if (vReal[k] > maxMag) {
				maxMag = vReal[k];
			}
		}
		double db = 20.0 * log10(maxMag / MAX_MAG + 1e-10);
		int height = (int)((db + 65.0) / 5.0);
		height = constrain(height, 0, 12);
		eqLevels[b] = (uint8_t)height;
	}
}

void drawEQGraphic(int x_start, int y) {
	int bar_width = 2;
	int spacing = 2;
	int max_height = 12;
	for (int i = 0; i < NUM_BARS; i++) {
		int bar_x = x_start + (i * (bar_width + spacing));
		int bar_h = eqLevels[i];
		int bar_y = y - bar_h;
		display.fillRect(bar_x, bar_y, bar_width, bar_h, SSD1306_WHITE);
	}
}

void updateDisplay() {
	display.clearDisplay();
	display.setTextSize(2);
	display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);

	const uint8_t SCROLL_LEFT_OFFSET = 2;
	const uint8_t VISIBLE_WIDTH = SCREEN_WIDTH - SCROLL_LEFT_OFFSET;

	unsigned long now = millis();

	display.fillRect(0, 0, SCREEN_WIDTH, 16, SSD1306_BLACK);

	updateEQGraphic();
	drawEQGraphic(2, 12);

	if (WiFi.status() == WL_CONNECTED) {
		drawWiFiBar(SCREEN_WIDTH - 20, 10, WiFi.RSSI());
	} else {
		display.setTextSize(1);
		display.setCursor(SCREEN_WIDTH - 40, 0);
		display.print("No WiFi");
		display.setTextSize(2);
	}

	String combinedText = currentSong.artist + " - " + currentSong.title;

	int16_t x, y;
	uint16_t w, h;
	display.getTextBounds(combinedText, 0, 16, &x, &y, &w, &h);

	if (w > VISIBLE_WIDTH) {
		const int16_t maxScrollPos = w - VISIBLE_WIDTH;

		display.setCursor(SCROLL_LEFT_OFFSET - titleScrollPos, 16);
		display.print(combinedText);

		bool isInitialDelayPassed = (now - songStartTime > SCROLL_START_DELAY_MS);

		if (isInitialDelayPassed && now - lastTitleScrollTime > SCROLL_STEP_DELAY_MS) {
			lastTitleScrollTime = now;

			if (titleScrollPos < maxScrollPos) {
				titleScrollPos++;
				titleScrollCompleteTime = 0;
			} else {
				if (titleScrollCompleteTime == 0) {
					titleScrollCompleteTime = now;
				}

				if (now - titleScrollCompleteTime > SCROLL_RESET_DELAY_MS) {
					titleScrollPos = 0;
					titleScrollCompleteTime = 0;
					songStartTime = now;
				}
			}
		}
	} else {
		titleScrollPos = 0;
		titleScrollCompleteTime = 0;
		display.setCursor(SCROLL_LEFT_OFFSET, 16);
		display.print(combinedText);
	}

	artistScrollPos = 0;
	artistScrollCompleteTime = 0;

	display.display();
}

void fetchNowPlaying() {
	if (WiFi.status() != WL_CONNECTED) {
		currentSong.artist = "No WiFi";
		currentSong.title = "Connecting...";
		return;
	}

	HTTPClient http;
	String url = "http://" + String(orangePiIP) + ":" + String(httpPort) + "/nowplaying";
	http.begin(url);

	int httpCode = http.GET();

	if (httpCode > 0) {
		if (httpCode == HTTP_CODE_OK) {
			String payload = http.getString();
			StaticJsonDocument<256> doc;
			DeserializationError error = deserializeJson(doc, payload);

			if (!error) {
				String newArtist = doc["artist"].as<String>();
				String newTitle = doc["title"].as<String>();

				if (newArtist != currentSong.artist || newTitle != currentSong.title) {
					currentSong.artist = newArtist;
					currentSong.title = newTitle;

					songStartTime = millis();
					artistScrollPos = 0;
					titleScrollPos = 0;
					artistScrollCompleteTime = 0;
					titleScrollCompleteTime = 0;
				}
			} else {
				currentSong.artist = "JSON Error";
				currentSong.title = "Check Server";
			}
		}
	} else {
		currentSong.artist = "Server Offline";
		currentSong.title = "TCP OK / HTTP Fail";
	}

	http.end();
}

void drawBootLogo(bool connecting) {
	display.clearDisplay();
	display.setTextColor(SSD1306_WHITE);

	display.setTextSize(2);
	display.setCursor(20, 0);
	display.print("STREAMER");

	display.setTextSize(1);
	display.setCursor(30, 24);

	if (connecting) {
		display.print("Connecting...");
	} else {
		display.print("Goodnight!");
	}

	int x_icon = 10;
	int y_icon = 24;
	display.fillRect(x_icon, y_icon + 6, 3, 2, SSD1306_WHITE);
	display.drawCircle(x_icon + 1, y_icon + 6, 4, SSD1306_WHITE);
	display.drawCircle(x_icon + 1, y_icon + 6, 7, SSD1306_WHITE);

	display.display();
}

void audioReadTask(void *parameter) {
	while (true) {
		if (!client.connected()) {
			if (!client.connect(orangePiIP, tcpPort)) {
				vTaskDelay(pdMS_TO_TICKS(5000));
				continue;
			}
		}

		int availableBytes = client.available();
		if (availableBytes > 0) {
			portENTER_CRITICAL(&bufferMutex);
			size_t freeSpace = AUDIO_BUFFER_SIZE - bytesInCircularBuffer;
			portEXIT_CRITICAL(&bufferMutex);

			size_t bytesToRead = (availableBytes < freeSpace) ? availableBytes : freeSpace;

			if (bytesToRead > 0) {
				size_t bytesToEnd = AUDIO_BUFFER_SIZE - bufferHead;
				size_t readPart1 = (bytesToRead < bytesToEnd) ? bytesToRead : bytesToEnd;
				size_t readBytes = client.read(audioBuffer + bufferHead, readPart1);

				if (readBytes > 0) {
					portENTER_CRITICAL(&bufferMutex);
					bufferHead = (bufferHead + readBytes) % AUDIO_BUFFER_SIZE;
					bytesInCircularBuffer += readBytes;
					portEXIT_CRITICAL(&bufferMutex);

					if (readBytes < bytesToRead) {
						size_t readPart2 = bytesToRead - readBytes;
						size_t readBytes2 = client.read(audioBuffer, readPart2);
						if (readBytes2 > 0) {
							portENTER_CRITICAL(&bufferMutex);
							bufferHead = (bufferHead + readBytes2) % AUDIO_BUFFER_SIZE;
							bytesInCircularBuffer += readBytes2;
							portEXIT_CRITICAL(&bufferMutex);
						}
					}
				}
			}
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void audioWriteTask(void *parameter) {
	size_t bytesAvailable;
	while (true) {
		portENTER_CRITICAL(&bufferMutex);
		bytesAvailable = bytesInCircularBuffer;
		portEXIT_CRITICAL(&bufferMutex);

		if (bytesAvailable >= MINIMUM_BUFFER_SIZE) {
			size_t bytesToEnd = AUDIO_BUFFER_SIZE - bufferTail;
			size_t bytesToWrite = (bytesAvailable < bytesToEnd) ? bytesAvailable : bytesToEnd;

			size_t written = 0;
			i2s_write(I2S_NUM, audioBuffer + bufferTail, bytesToWrite, &written, pdMS_TO_TICKS(10));

			if (written > 0) {
				portENTER_CRITICAL(&bufferMutex);
				bufferTail = (bufferTail + written) % AUDIO_BUFFER_SIZE;
				bytesInCircularBuffer -= written;
				portEXIT_CRITICAL(&bufferMutex);
			}
		} else {
			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}
}

void displayTask(void *parameter) {
	uint32_t fetchCounter = 0;
	const uint32_t fetchInterval = 5000 / 15;

	while (true) {
		if (isDisplayActive) {
			if (fetchCounter >= fetchInterval) {
				fetchNowPlaying();
				fetchCounter = 0;
			}

			updateDisplay();
			fetchCounter++;
		}
		vTaskDelay(pdMS_TO_TICKS(15));
	}
}

void sleepTask(void *parameter) {
	while (gpio_get_level(GPIO_SENSOR_PIN) == 0) {
		vTaskDelay(pdMS_TO_TICKS(100));
	}

	vTaskDelay(pdMS_TO_TICKS(100));

	auto updateStatus = [&](const String& msg) {
		display.fillRect(0, 16, SCREEN_WIDTH, SCREEN_HEIGHT - 16, SSD1306_BLACK);
		display.setCursor(0, 16);
		display.print(msg);
		display.display();
	};

	if (displayTaskHandle != NULL) {
		isDisplayActive = false;
		vTaskDelay(pdMS_TO_TICKS(50));
	}

	drawBootLogo(false);
	vTaskDelay(pdMS_TO_TICKS(500));

	updateStatus("Stop I2S...");
	vTaskDelay(pdMS_TO_TICKS(200));
	i2s_stop(I2S_NUM);

	updateStatus("Stop TCP...");
	vTaskDelay(pdMS_TO_TICKS(200));
	client.stop();

	updateStatus("Del tasks...");
	vTaskDelay(pdMS_TO_TICKS(200));

	if (displayTaskHandle != NULL) {
		vTaskDelete(displayTaskHandle);
		displayTaskHandle = NULL;
	}

	if (readTaskHandle != NULL) {
		vTaskDelete(readTaskHandle);
		readTaskHandle = NULL;
	}
	if (writeTaskHandle != NULL) {
		vTaskDelete(writeTaskHandle);
		writeTaskHandle = NULL;
	}

	updateStatus("Send restart...");
	vTaskDelay(pdMS_TO_TICKS(200));
	if (WiFi.status() == WL_CONNECTED) {
		HTTPClient http;
		String url = "http://" + String(orangePiIP) + ":" + String(httpPort) + "/restart";
		http.begin(url);
		http.setConnectTimeout(500);
		http.setTimeout(1000);
		http.GET();
		http.end();
	}

	updateStatus("Shut WiFi...");
	vTaskDelay(pdMS_TO_TICKS(200));
	WiFi.disconnect(true);
	WiFi.mode(WIFI_OFF);
	vTaskDelay(pdMS_TO_TICKS(50));

	updateStatus("Deinit HW...");
	vTaskDelay(pdMS_TO_TICKS(200));

	i2s_driver_uninstall(I2S_NUM);

	gpio_config_t stream_out_config = {
		.pin_bit_mask = (1ULL << GPIO_STREAM_IND_PIN),
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config(&stream_out_config);

	gpio_set_level(GPIO_STATE_IND_PIN, 1);
	gpio_set_level(GPIO_STREAM_IND_PIN, 0);
	gpio_hold_en(GPIO_STREAM_IND_PIN);
	gpio_hold_en(GPIO_STATE_IND_PIN);

	updateStatus("Entering sleep..");
	vTaskDelay(pdMS_TO_TICKS(500));

	display.ssd1306_command(SSD1306_DISPLAYOFF);
	Wire.end();

	esp_deep_sleep_enable_gpio_wakeup(1ULL << GPIO_SENSOR_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH);

	esp_deep_sleep_start();
}


void setup() {

	gpio_hold_dis(GPIO_STATE_IND_PIN);
	gpio_hold_dis(GPIO_STREAM_IND_PIN);

	gpio_config_t ind_io_config = {
		.pin_bit_mask = (1ULL << GPIO_STATE_IND_PIN) | (1ULL << GPIO_STREAM_IND_PIN),
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config(&ind_io_config);

	gpio_set_level(GPIO_STATE_IND_PIN, 0);
	gpio_set_level(GPIO_STREAM_IND_PIN, 0);

	gpio_config_t io_config = {
		.pin_bit_mask = (1ULL << GPIO_SENSOR_PIN),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_ENABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config(&io_config);

	Wire.begin(OLED_SDA, OLED_SCL);

	if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
	}
	display.display();
	display.clearDisplay();
	display.setTextSize(1);
	display.setTextColor(SSD1306_WHITE);
	display.setTextWrap(false);
	
	drawBootLogo(true);

	WiFi.begin(ssid);
	while (WiFi.status() != WL_CONNECTED) {
		vTaskDelay(pdMS_TO_TICKS(500));
	}

	if (!client.connect(orangePiIP, tcpPort)) {
		return;
	}

	i2s_config_t i2s_config = {
		.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
		.sample_rate = 44100,
		.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
		.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
		.communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S),
		.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
		.dma_buf_count = 8,
		.dma_buf_len = 1024,
		.use_apll = false,
		.tx_desc_auto_clear = true
	};

	i2s_pin_config_t pin_config = {
		.bck_io_num = I2S_BCK_IO,
		.ws_io_num = I2S_WS_IO,
		.data_out_num = I2S_DOUT_IO,
		.data_in_num = I2S_PIN_NO_CHANGE
	};

	i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
	i2s_set_pin(I2S_NUM, &pin_config);

	gpio_config_t stream_in_config = {
		.pin_bit_mask = (1ULL << GPIO_STREAM_IND_PIN),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config(&stream_in_config);

	xTaskCreate(
		audioReadTask,
		"Audio Read Task",
		4096,
		NULL,
		1,
		&readTaskHandle
	);

	xTaskCreate(
		audioWriteTask,
		"Audio Write Task",
		4096,
		NULL,
		2,
		&writeTaskHandle
	);

	xTaskCreate(
		displayTask,
		"Display Task",
		4096,
		NULL,
		3,
		&displayTaskHandle
	);

	xTaskCreate(
		sleepTask,
		"Sleep Task",
		4096,
		NULL,
		3,
		NULL
	);
}

void loop() {
	vTaskDelay(pdMS_TO_TICKS(1000));
}
