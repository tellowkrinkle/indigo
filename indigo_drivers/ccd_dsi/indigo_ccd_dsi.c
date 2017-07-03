// Copyright (c) 2017 Rumen G. Bogdanovski
// All rights reserved.
//
// You can use this software under the terms of 'INDIGO Astronomy
// open-source license' (see LICENSE.md).
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHORS 'AS IS' AND ANY EXPRESS
// OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
// GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// version history
// 2.0 by Rumen Bogdanovski <rumen@skyarchive.org>


/** INDIGO CCD driver for Meade DSI
 \file indigo_ccd_dsi.c
 */

#define DRIVER_VERSION 0x0001
#define DRIVER_NAME		"indigo_ccd_dsi"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>

#include <pthread.h>
#include <sys/time.h>

#if defined(INDIGO_MACOS)
#include <libusb-1.0/libusb.h>
#elif defined(INDIGO_FREEBSD)
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif


#define MAX_CCD_TEMP         45     /* Max CCD temperature */
#define MIN_CCD_TEMP        -55     /* Min CCD temperature */
#define MAX_X_BIN            1      /* Max Horizontal binning */
#define MAX_Y_BIN            1      /* Max Vertical binning */

#define DEFAULT_BPP          16     /* Default bits per pixel */
#define MAX_PATH            255     /* Maximal Path Length */

#define TEMP_CHECK_TIME       3     /* Time between teperature checks (seconds) */

#include "libdsi.h"
#include "indigo_driver_xml.h"
#include "indigo_ccd_dsi.h"

#define DSI_VENDOR_ID              0x156c

#define MAX_MODES                  32

#define PRIVATE_DATA               ((dsi_private_data *)device->private_data)

// gp_bits is used as boolean
#define is_connected               gp_bits

#undef INDIGO_DEBUG_DRIVER
#define INDIGO_DEBUG_DRIVER(c) c

// -------------------------------------------------------------------------------- FLI USB interface implementation

#define ms2s(s)      ((s) / 1000.0)
#define s2ms(ms)     ((ms) * 1000)

typedef struct {
	char dev_sid[DSI_ID_LEN];
	dsi_camera_t *dsi;
	int count_open;
	indigo_timer *exposure_timer, *temperature_timer;
	long int buffer_size;
	char *buffer;
	pthread_mutex_t usb_mutex;
	bool can_check_temperature;
} dsi_private_data;


static bool camera_open(indigo_device *device) {
	int id;

	if (device->is_connected) return false;

	pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);

	PRIVATE_DATA->dsi = dsi_open_camera(PRIVATE_DATA->dev_sid);
	if (PRIVATE_DATA->dsi == NULL) {
		pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "dsi_open_camera(%s) = %p", PRIVATE_DATA->dev_sid, PRIVATE_DATA->dsi);
		return false;
	}

	if (PRIVATE_DATA->buffer == NULL) {
		PRIVATE_DATA->buffer_size = dsi_get_image_width(PRIVATE_DATA->dsi) *
		                            dsi_get_image_height(PRIVATE_DATA->dsi) *
		                            dsi_get_bytespp(PRIVATE_DATA->dsi) +
		                            FITS_HEADER_SIZE;
		PRIVATE_DATA->buffer = (unsigned char*)indigo_alloc_blob_buffer(PRIVATE_DATA->buffer_size);
		if (PRIVATE_DATA->buffer == NULL) {
			dsi_close_camera(PRIVATE_DATA->dsi);
			PRIVATE_DATA->dsi = NULL;
			pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
			return true;
		}
	}

	pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
	return true;
}


static bool camera_start_exposure(indigo_device *device, double exposure, bool dark) {
	long res;

	pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);

	res = dsi_start_exposure(PRIVATE_DATA->dsi, exposure);
	if (res) {
		pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "dsi_start_exposure(%s) = %d", PRIVATE_DATA->dev_sid, res);
		return false;
	}

	pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
	return true;
}


static bool camera_read_pixels(indigo_device *device) {
	long res;
	dsi_set_image_little_endian(PRIVATE_DATA->dsi, 0);
	while ((res = dsi_read_image(PRIVATE_DATA->dsi, (unsigned char*)(PRIVATE_DATA->buffer + FITS_HEADER_SIZE), O_NONBLOCK)) != 0) {
		if (res == EWOULDBLOCK) {
			double time_left = dsi_get_exposure_time_left(PRIVATE_DATA->dsi);
			INDIGO_DRIVER_DEBUG(stderr, "Image not ready, sleeping for %.3fs...\n", time_left);
			usleep((int)(time_left*1000000));
		} else {
			INDIGO_DRIVER_ERROR(DRIVER_NAME, "Exposure Failed! dsi_read_image(%s) = %d", PRIVATE_DATA->dev_sid, res);
			dsi_abort_exposure(PRIVATE_DATA->dsi);
			dsi_reset_camera(PRIVATE_DATA->dsi);
			return false;
		}
	}
	return true;
}


static bool camera_abort_exposure(indigo_device *device) {
	pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);

	dsi_abort_exposure(PRIVATE_DATA->dsi);
	PRIVATE_DATA->can_check_temperature = true;

	pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
	return true;
}


static void camera_close(indigo_device *device) {
	if (!device->is_connected) return;

	pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);
	dsi_close_camera(PRIVATE_DATA->dsi);
	pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
	if (PRIVATE_DATA->buffer != NULL) {
		free(PRIVATE_DATA->buffer);
		PRIVATE_DATA->buffer = NULL;
	}
}

// -------------------------------------------------------------------------------- INDIGO CCD device implementation

// callback for image download
static void exposure_timer_callback(indigo_device *device) {
	PRIVATE_DATA->exposure_timer = NULL;
	if (!CONNECTION_CONNECTED_ITEM->sw.value) return;
	if (CCD_EXPOSURE_PROPERTY->state == INDIGO_BUSY_STATE) {
		CCD_EXPOSURE_ITEM->number.value = 0;
		indigo_update_property(device, CCD_EXPOSURE_PROPERTY, NULL);
		if (camera_read_pixels(device)) {
			const char *color_string = dsi_get_bayer_pattern(PRIVATE_DATA->dsi);
			if (color_string[0] != '\0') {
				/* NOTE: There is no need to take care about the offsets,
				   the SDK takes care the image to be in the correct bayer pattern */
				indigo_fits_keyword keywords[] = {
					{ INDIGO_FITS_STRING, "BAYERPAT", .string = color_string, "Bayer color pattern" },
					{ INDIGO_FITS_NUMBER, "XBAYROFF", .number = 0, "X offset of Bayer array" },
					{ INDIGO_FITS_NUMBER, "YBAYROFF", .number = 0, "Y offset of Bayer array" },
					{ 0 }
				};
				indigo_process_image(
					device,
					PRIVATE_DATA->buffer,
					(int)(CCD_FRAME_WIDTH_ITEM->number.value / CCD_BIN_HORIZONTAL_ITEM->number.value),
					(int)(CCD_FRAME_HEIGHT_ITEM->number.value / CCD_BIN_VERTICAL_ITEM->number.value),
					false, keywords
				);
			} else {
				indigo_process_image(
					device,
					PRIVATE_DATA->buffer,
					(int)(CCD_FRAME_WIDTH_ITEM->number.value / CCD_BIN_HORIZONTAL_ITEM->number.value),
					(int)(CCD_FRAME_HEIGHT_ITEM->number.value / CCD_BIN_VERTICAL_ITEM->number.value),
					false, NULL
				);
			}
			CCD_EXPOSURE_PROPERTY->state = INDIGO_OK_STATE;
			indigo_update_property(device, CCD_EXPOSURE_PROPERTY, NULL);
		} else {
			CCD_EXPOSURE_PROPERTY->state = INDIGO_ALERT_STATE;
			indigo_update_property(device, CCD_EXPOSURE_PROPERTY, "Exposure failed");
		}
	}
	PRIVATE_DATA->can_check_temperature = true;
}


// callback called 4s before image download (e.g. to clear vreg or turn off temperature check)
static void clear_reg_timer_callback(indigo_device *device) {
	if (!CONNECTION_CONNECTED_ITEM->sw.value) return;
	if (CCD_EXPOSURE_PROPERTY->state == INDIGO_BUSY_STATE) {
		PRIVATE_DATA->can_check_temperature = false;
		PRIVATE_DATA->exposure_timer = indigo_set_timer(device, 4, exposure_timer_callback);
	} else {
		PRIVATE_DATA->exposure_timer = NULL;
	}
}


static void ccd_temperature_callback(indigo_device *device) {
	if (!CONNECTION_CONNECTED_ITEM->sw.value) return;
	if (PRIVATE_DATA->can_check_temperature) {
		// check temperature;
		CCD_TEMPERATURE_ITEM->number.value = dsi_get_temperature(PRIVATE_DATA->dsi);
		indigo_update_property(device, CCD_TEMPERATURE_PROPERTY, NULL);
	}
	indigo_reschedule_timer(device, TEMP_CHECK_TIME, &PRIVATE_DATA->temperature_timer);
}


static indigo_result ccd_attach(indigo_device *device) {
	assert(device != NULL);
	assert(PRIVATE_DATA != NULL);
	if (indigo_ccd_attach(device, DRIVER_VERSION) == INDIGO_OK) {
		pthread_mutex_init(&PRIVATE_DATA->usb_mutex, NULL);

		/* Use all info property fields */
		INFO_PROPERTY->count = 7;

		return indigo_ccd_enumerate_properties(device, NULL, NULL);
	}
	return INDIGO_FAILED;
}


static bool handle_exposure_property(indigo_device *device, indigo_property *property) {
	long ok;

	ok = camera_start_exposure(device, CCD_EXPOSURE_ITEM->number.target, CCD_FRAME_TYPE_DARK_ITEM->sw.value || CCD_FRAME_TYPE_BIAS_ITEM->sw.value);

	if (ok) {
		if (CCD_UPLOAD_MODE_LOCAL_ITEM->sw.value) {
			CCD_IMAGE_FILE_PROPERTY->state = INDIGO_BUSY_STATE;
			indigo_update_property(device, CCD_IMAGE_FILE_PROPERTY, NULL);
		} else {
			CCD_IMAGE_PROPERTY->state = INDIGO_BUSY_STATE;
			indigo_update_property(device, CCD_IMAGE_PROPERTY, NULL);
		}

		CCD_EXPOSURE_PROPERTY->state = INDIGO_BUSY_STATE;

		indigo_update_property(device, CCD_EXPOSURE_PROPERTY, NULL);
		if (CCD_EXPOSURE_ITEM->number.target > 4) {
			PRIVATE_DATA->exposure_timer = indigo_set_timer(device, CCD_EXPOSURE_ITEM->number.target - 4, clear_reg_timer_callback);
		} else {
			PRIVATE_DATA->can_check_temperature = false;
			PRIVATE_DATA->exposure_timer = indigo_set_timer(device, CCD_EXPOSURE_ITEM->number.target, exposure_timer_callback);
		}

	} else {
		CCD_EXPOSURE_PROPERTY->state = INDIGO_ALERT_STATE;
		indigo_update_property(device, CCD_EXPOSURE_PROPERTY, "Exposure failed.");
	}
	return false;
}

static indigo_result ccd_change_property(indigo_device *device, indigo_client *client, indigo_property *property) {
	assert(device != NULL);
	assert(DEVICE_CONTEXT != NULL);
	assert(property != NULL);

	// -------------------------------------------------------------------------------- CONNECTION -> CCD_INFO, CCD_COOLER, CCD_TEMPERATURE
	if (indigo_property_match(CONNECTION_PROPERTY, property)) {
		indigo_property_copy_values(CONNECTION_PROPERTY, property, false);
		if (CONNECTION_CONNECTED_ITEM->sw.value) {
			if (!device->is_connected) {
				if (camera_open(device)) {
					int id = 0;
					long res;
					int i;
					char mode_name[INDIGO_NAME_SIZE];

					pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);
					CCD_INFO_WIDTH_ITEM->number.value = dsi_get_image_width(PRIVATE_DATA->dsi);
					CCD_INFO_HEIGHT_ITEM->number.value = dsi_get_image_height(PRIVATE_DATA->dsi);
					CCD_FRAME_WIDTH_ITEM->number.value = CCD_FRAME_WIDTH_ITEM->number.max = CCD_FRAME_LEFT_ITEM->number.max = CCD_INFO_WIDTH_ITEM->number.value;
					CCD_FRAME_HEIGHT_ITEM->number.value = CCD_FRAME_HEIGHT_ITEM->number.max = CCD_FRAME_TOP_ITEM->number.max = CCD_INFO_HEIGHT_ITEM->number.value;

					//sprintf(INFO_DEVICE_FW_REVISION_ITEM->text.value, "%ld", fw_rev);
					//sprintf(INFO_DEVICE_HW_REVISION_ITEM->text.value, "%ld", hw_rev);
					sprintf(INFO_DEVICE_SERIAL_NUM_ITEM->text.value, "%s", dsi_get_serial_number(PRIVATE_DATA->dsi));
					sprintf(INFO_DEVICE_MODEL_ITEM->text.value, "%s", dsi_get_model_name(PRIVATE_DATA->dsi));

					indigo_update_property(device, INFO_PROPERTY, NULL);

					CCD_INFO_PIXEL_WIDTH_ITEM->number.value = dsi_get_pixel_width(PRIVATE_DATA->dsi);
					CCD_INFO_PIXEL_HEIGHT_ITEM->number.value = dsi_get_pixel_height(PRIVATE_DATA->dsi);
					CCD_INFO_PIXEL_SIZE_ITEM->number.value = CCD_INFO_PIXEL_WIDTH_ITEM->number.value;
					CCD_INFO_MAX_HORIZONAL_BIN_ITEM->number.value = MAX_X_BIN;
					CCD_INFO_MAX_VERTICAL_BIN_ITEM->number.value = MAX_Y_BIN;

					CCD_FRAME_PROPERTY->perm = INDIGO_RO_PERM;
					CCD_FRAME_BITS_PER_PIXEL_ITEM->number.value = DEFAULT_BPP;
					CCD_FRAME_BITS_PER_PIXEL_ITEM->number.min = DEFAULT_BPP;
					CCD_FRAME_BITS_PER_PIXEL_ITEM->number.max = DEFAULT_BPP;

					CCD_BIN_PROPERTY->hidden = true;  // keep it hidden as we do not support binning yet!
					CCD_BIN_PROPERTY->perm = INDIGO_RO_PERM;
					CCD_BIN_HORIZONTAL_ITEM->number.value = CCD_BIN_HORIZONTAL_ITEM->number.min = 1;
					CCD_BIN_HORIZONTAL_ITEM->number.max = MAX_X_BIN;
					CCD_BIN_VERTICAL_ITEM->number.value = CCD_BIN_VERTICAL_ITEM->number.min = 1;
					CCD_BIN_VERTICAL_ITEM->number.max = MAX_Y_BIN;

					CCD_INFO_BITS_PER_PIXEL_ITEM->number.value = DEFAULT_BPP;

					CCD_TEMPERATURE_PROPERTY->hidden = false;
					CCD_TEMPERATURE_PROPERTY->perm = INDIGO_RO_PERM;
					CCD_TEMPERATURE_ITEM->number.min = MIN_CCD_TEMP;
					CCD_TEMPERATURE_ITEM->number.max = MAX_CCD_TEMP;
					CCD_TEMPERATURE_ITEM->number.step = 0;

					CCD_GAIN_PROPERTY->hidden = false;
					CCD_GAIN_PROPERTY->perm = INDIGO_RW_PERM;
					CCD_GAIN_ITEM->number.min = 0;
					CCD_GAIN_ITEM->number.max = 100;
					CCD_GAIN_ITEM->number.value = dsi_get_amp_gain(PRIVATE_DATA->dsi);

					CCD_OFFSET_PROPERTY->hidden = false;
					CCD_OFFSET_PROPERTY->perm = INDIGO_RW_PERM;
					CCD_OFFSET_ITEM->number.min = 0;
					CCD_OFFSET_ITEM->number.max = 100;
					CCD_OFFSET_ITEM->number.value = dsi_get_amp_offset(PRIVATE_DATA->dsi);

					double temp = dsi_get_temperature(PRIVATE_DATA->dsi);
					pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
					if (temp > 1000) {  /* no sensor */
						CCD_TEMPERATURE_PROPERTY->hidden = true;
						INDIGO_DRIVER_DEBUG(DRIVER_NAME, "dsi_get_temperature(%s) = NO_SENSOR", PRIVATE_DATA->dev_sid);
					} else {
						PRIVATE_DATA->can_check_temperature = true;
						PRIVATE_DATA->temperature_timer = indigo_set_timer(device, 0, ccd_temperature_callback);
					}

					device->is_connected = true;
					CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
				} else {
					CONNECTION_PROPERTY->state = INDIGO_ALERT_STATE;
					indigo_set_switch(CONNECTION_PROPERTY, CONNECTION_DISCONNECTED_ITEM, true);
					indigo_set_switch(CONNECTION_PROPERTY, CONNECTION_CONNECTED_ITEM, false);
					indigo_update_property(device, CONNECTION_PROPERTY, NULL);
					return INDIGO_FAILED;
				}
			}
		} else {
			if (device->is_connected) {
				PRIVATE_DATA->can_check_temperature = false;
				indigo_cancel_timer(device, &PRIVATE_DATA->temperature_timer);
				camera_close(device);
				device->is_connected = false;
				CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
			}
		}
	// -------------------------------------------------------------------------------- CCD_EXPOSURE
	} else if (indigo_property_match(CCD_EXPOSURE_PROPERTY, property)) {
		if (CCD_EXPOSURE_PROPERTY->state == INDIGO_BUSY_STATE)
			return INDIGO_OK;
		indigo_property_copy_values(CCD_EXPOSURE_PROPERTY, property, false);
		if (IS_CONNECTED) {
			handle_exposure_property(device, property);
		}
	// -------------------------------------------------------------------------------- CCD_ABORT_EXPOSURE
	} else if (indigo_property_match(CCD_ABORT_EXPOSURE_PROPERTY, property)) {
		if (CCD_EXPOSURE_PROPERTY->state == INDIGO_BUSY_STATE) {
			camera_abort_exposure(device);
		}
		PRIVATE_DATA->can_check_temperature = true;
		indigo_property_copy_values(CCD_ABORT_EXPOSURE_PROPERTY, property, false);
	// ------------------------------------------------------------------------------- GAIN
	} else if (indigo_property_match(CCD_GAIN_PROPERTY, property)) {
		CCD_GAIN_PROPERTY->state = INDIGO_IDLE_STATE;
		indigo_property_copy_values(CCD_GAIN_PROPERTY, property, false);

		pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);
		dsi_set_amp_gain(PRIVATE_DATA->dsi, (int)(CCD_GAIN_ITEM->number.value));
		pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);

		CCD_GAIN_PROPERTY->state = INDIGO_OK_STATE;
		if (IS_CONNECTED)
			indigo_update_property(device, CCD_GAIN_PROPERTY, NULL);
		return INDIGO_OK;
	// ------------------------------------------------------------------------------- OFFSET
} else if (indigo_property_match(CCD_OFFSET_PROPERTY, property)) {
		CCD_OFFSET_PROPERTY->state = INDIGO_IDLE_STATE;
		indigo_property_copy_values(CCD_OFFSET_PROPERTY, property, false);

		pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);
		dsi_set_amp_offset(PRIVATE_DATA->dsi, (int)(CCD_OFFSET_ITEM->number.value));
		pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);

		CCD_OFFSET_PROPERTY->state = INDIGO_OK_STATE;
		if (IS_CONNECTED)
			indigo_update_property(device, CCD_OFFSET_PROPERTY, NULL);
		return INDIGO_OK;
	// -------------------------------------------------------------------------------- CONFIG
	} else if (indigo_property_match(CONFIG_PROPERTY, property)) {
		if (indigo_switch_match(CONFIG_SAVE_ITEM, property)) {
		}
	}
	// -----------------------------------------------------------------------------
	return indigo_ccd_change_property(device, client, property);
}


static indigo_result ccd_detach(indigo_device *device) {
	assert(device != NULL);
	if (CONNECTION_CONNECTED_ITEM->sw.value)
		indigo_device_disconnect(NULL, device->name);

	INDIGO_DRIVER_LOG(DRIVER_NAME, "'%s' detached.", device->name);

	return indigo_ccd_detach(device);
}


// -------------------------------------------------------------------------------- hot-plug support
static pthread_mutex_t device_mutex = PTHREAD_MUTEX_INITIALIZER;

#define MAX_DEVICES                   32
#define NOT_FOUND                    (-1)

static indigo_device *devices[MAX_DEVICES] = {NULL};


static bool find_plugged_device_sid(char *new_sid) {
	int i;
	char sid[DSI_ID_LEN] = {0};
	bool found = false;
	dsi_device_list dev_list;

	int count = dsi_scan_usb(dev_list);
	for(i = 0; i < count; i++) {
		INDIGO_DRIVER_DEBUG(DRIVER_NAME,"+ %d of %d: %s", i , count, dev_list[i]);
		found = false;
		for(int slot = 0; slot < MAX_DEVICES; slot++) {
			indigo_device *device = devices[slot];
			if (device == NULL) continue;
			if (PRIVATE_DATA && (!strncmp(PRIVATE_DATA->dev_sid, dev_list[i], DSI_ID_LEN))) {
				found = true;
				break;
			}
		}

		if (!found) {
			strncpy(new_sid, dev_list[i], DSI_ID_LEN);
			return true;
		}
	}
	new_sid[0] = '\0';
	return false;
}


static int find_available_device_slot() {
	for(int slot = 0; slot < MAX_DEVICES; slot++) {
		if (devices[slot] == NULL) return slot;
	}
	return NOT_FOUND;
}


static int find_device_slot(const char *sid) {
	for(int slot = 0; slot < MAX_DEVICES; slot++) {
		indigo_device *device = devices[slot];
		if (device == NULL) continue;
		if (!strncmp(PRIVATE_DATA->dev_sid, sid, DSI_ID_LEN)) return slot;
	}
	return NOT_FOUND;
}


static int find_unplugged_device_slot() {
	int slot;
	indigo_device *device;
	char sid[DSI_ID_LEN] = {0};
	bool found = true;
	dsi_device_list dev_list;

	int count = dsi_scan_usb(dev_list);
	for(slot = 0; slot < MAX_DEVICES; slot++) {
		device = devices[slot];
		if (device == NULL) continue;
		found = false;
		for(int i = 0; i < count; i++) {
			INDIGO_DRIVER_DEBUG(DRIVER_NAME,"- %d of %d: %s", i , count, dev_list[i]);
			if (PRIVATE_DATA && (!strncmp(PRIVATE_DATA->dev_sid, dev_list[i], DSI_ID_LEN))) {
				found = true;
				break;
			}
		}
		if (!found) return slot;
	}
	return NOT_FOUND;
}


static void process_plug_event() {
	static indigo_device ccd_template = {
		"", false, NULL, NULL, INDIGO_OK, INDIGO_VERSION_CURRENT,
		ccd_attach,
		indigo_ccd_enumerate_properties,
		ccd_change_property,
		NULL,
		ccd_detach
	};

	int slot = find_available_device_slot();
	if (slot < 0) {
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "No device slots available.");
		return;
	}

	char sid[DSI_ID_LEN];
	bool found = find_plugged_device_sid(sid);
	if (!found) {
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "No plugged device found.");
		return;
	}

	char dev_name[DSI_NAME_LEN];
	dsi_camera_t *dsi;
	dsi = dsi_open_camera(sid);
	if(dsi == NULL) {
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Camera %s can not be open.", sid);
		return;
	}
	strncpy(dev_name, dsi_get_model_name(dsi), DSI_NAME_LEN);
	dsi_close_camera(dsi);

	indigo_device *device = (indigo_device*)malloc(sizeof(indigo_device));
	assert(device != NULL);
	memcpy(device, &ccd_template, sizeof(indigo_device));
	sprintf(device->name, "%s #%s", dev_name, sid);
	INDIGO_DRIVER_LOG(DRIVER_NAME, "'%s' attached.", device->name);
	dsi_private_data *private_data = (dsi_private_data*)malloc(sizeof(dsi_private_data));
	assert(private_data);
	memset(private_data, 0, sizeof(dsi_private_data));
	sprintf(private_data->dev_sid, "%s", sid);
	device->private_data = private_data;
	indigo_async((void *(*)(void *))indigo_attach_device, device);
	devices[slot]=device;
}


static void process_unplug_event() {
	int slot;
	bool removed = false;
	dsi_private_data *private_data = NULL;
	while ((slot = find_unplugged_device_slot()) != NOT_FOUND) {
		indigo_device **device = &devices[slot];
		if (*device == NULL) {
			return;
		}
		indigo_detach_device(*device);
		if ((*device)->private_data) {
			private_data = (dsi_private_data*)((*device)->private_data);
		}
		free(*device);
		*device = NULL;
		removed = true;
	}

	if (private_data) {
		dsi_close_camera(private_data->dsi);
		free(private_data);
		private_data = NULL;
	}

	if (!removed) {
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "No DSI Camera unplugged!");
	}
}


#ifdef __APPLE__
void *plug_thread_func(void *sid) {
	pthread_mutex_lock(&device_mutex);
	dsi_load_firmware();
	process_plug_event();
	pthread_mutex_unlock(&device_mutex);
	pthread_exit(NULL);
	return NULL;
}


void *unplug_thread_func(void *sid) {
	pthread_mutex_lock(&device_mutex);
	process_unplug_event();
	pthread_mutex_unlock(&device_mutex);
	pthread_exit(NULL);
	return NULL;
}
#endif /* __APPLE__ */


static int hotplug_callback(libusb_context *ctx, libusb_device *dev, libusb_hotplug_event event, void *user_data) {
	struct libusb_device_descriptor descriptor;

	pthread_mutex_lock(&device_mutex);
	libusb_get_device_descriptor(dev, &descriptor);
	if (descriptor.idVendor != DSI_VENDOR_ID) {
		pthread_mutex_unlock(&device_mutex);
		return 0;
	}
	INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Hotplug: vid=%x pid=%x", descriptor.idVendor, descriptor.idProduct);
	switch (event) {
		case LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED: {
			#ifdef __APPLE__
				pthread_t plug_thread;
				/* This is ugly hack but otherwise does not work!!!
				   The camera does not respond in the hotpliug callback on MacOS,
				   so the thread waits the callback to complete and initializes
				   the camera.
				 */
				if (pthread_create(&plug_thread, NULL, plug_thread_func, NULL)) {
					INDIGO_DRIVER_ERROR(DRIVER_NAME,"Error creating thread for firmware loader");
				}
			#else
				process_plug_event();
			#endif /* __APPLE__ */
			break;
		}
		case LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT: {
			#ifdef __APPLE__
				pthread_t unplug_thread;
				/* This is ugly hack but otherwise does not work!!!
				   See the note in LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED case.
				*/
				if (pthread_create(&unplug_thread, NULL, unplug_thread_func, NULL)) {
					INDIGO_DRIVER_ERROR(DRIVER_NAME,"Error creating thread for firmware loader");
				}
			#else
				process_unplug_event();
			#endif /* __APPLE__ */
			break;
		}
	}
	pthread_mutex_unlock(&device_mutex);
	return 0;
};


static void remove_all_devices() {
	int i;
	for(i = 0; i < MAX_DEVICES; i++) {
		indigo_device **device = &devices[i];
		if (*device == NULL)
			continue;
		indigo_detach_device(*device);
		free((*device)->private_data);
		free(*device);
		*device = NULL;
	}
}


static libusb_hotplug_callback_handle callback_handle;

indigo_result indigo_ccd_dsi(indigo_driver_action action, indigo_driver_info *info) {
	static indigo_driver_action last_action = INDIGO_DRIVER_SHUTDOWN;

	SET_DRIVER_INFO(info, "Meade DSI Camera", __FUNCTION__, DRIVER_VERSION, last_action);

	if (action == last_action)
		return INDIGO_OK;

	switch (action) {
	case INDIGO_DRIVER_INIT:
		last_action = action;
		indigo_start_usb_event_handler();
		int rc = libusb_hotplug_register_callback(NULL, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT, LIBUSB_HOTPLUG_ENUMERATE, DSI_VENDOR_ID, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, hotplug_callback, NULL, &callback_handle);
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "libusb_hotplug_register_callback ->  %s", rc < 0 ? libusb_error_name(rc) : "OK");
		return rc >= 0 ? INDIGO_OK : INDIGO_FAILED;

	case INDIGO_DRIVER_SHUTDOWN:
		last_action = action;
		libusb_hotplug_deregister_callback(NULL, callback_handle);
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "libusb_hotplug_deregister_callback");
		remove_all_devices();
		break;

	case INDIGO_DRIVER_INFO:
		break;
	}

	return INDIGO_OK;
}