#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <memory.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <linux/input.h>
#include <linux/uinput.h>

#include <functional>
#include <unordered_set>
#include <vector>

#include <dbus/dbus.h>

#include "shared/dbus_helpers.hpp"
#include "shared/prevent_brick.hpp"

#define HMI_BUS_ADDRESS "unix:path=/tmp/dbus_hmi_socket"
#define SERVICE_BUS_ADDRESS "unix:path=/tmp/dbus_service_socket"

#define AUDIO_AA 13
#define AUDIO_RADIO 6

constexpr char source_device[] = "/dev/input/event1";
constexpr char filtered_name[] = "Virtual Keyboard";

// HACK: There doesn't seem to be a good way to get the path of the device
//       The UI_GET_SYSNAME ioctl requires kernel 3.15+
constexpr char new_device_path[] = "/dev/input/event6";

static int infd = -1;
static int outfd = -1;

using matcher_fn_t = std::function<bool (const struct input_event *)>;
static std::vector<matcher_fn_t> matchers;

int audio_status;

static void switch_audio(int sessionId)
{
	DBusConnection *service_bus;
	DBusError error;
	DBusMessageIter args;

	audio_status = sessionId;

	service_bus = dbus_connection_open(SERVICE_BUS_ADDRESS, &error);

	if (!service_bus) {
		printf("DBUS: failed to connect to service bus: %s: %s\n", error.name, error.message);
	}

	if (!dbus_bus_register(service_bus, &error)) {
		printf("DBUS: failed to register with service bus: %s: %s\n", error.name, error.message);
	}

	DBusMessage *msg = dbus_message_new_method_call("com.xsembedded.service.AudioManagement", "/com/xse/service/AudioManagement/AudioApplication", "com.xsembedded.ServiceProvider", "Request");
	if (msg == NULL)
	{
		printf("DBUS: msg null\n");
		return;
	}

	dbus_message_iter_init_append(msg, &args);
	char* action = "requestAudioFocus";
	if(!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &action))
	{
		printf("DBUS: iter append failed\n");
		return;
	}
	char* session = (char*) malloc(30);
	snprintf(session, 30, "{\"sessionId\":%d}", sessionId);
	puts(session);
	if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &session))
	{
		printf("DBUS: iter append failed 2\n");
		return;
	}

	if (!dbus_connection_send(service_bus, msg, 0))
	{
		printf("DBUS: send failed\n");
		return;
	}

	dbus_connection_flush(service_bus);
	dbus_message_unref(msg);
	dbus_connection_close(service_bus);
}

std::unordered_set<int> captured_keys_aa_audio = {
	KEY_G, //Steering wheel talk
	KEY_LEFTBRACE, //Steering wheel next track
	KEY_RIGHTBRACE, //Steering wheel prev track
	KEY_E, //Music button
	KEY_HOME, //Home button
	KEY_R, //Nav button
	KEY_BACKSPACE, //Back button
	KEY_T, //Fav button
	KEY_ENTER, //Commander depress
	KEY_LEFT, //Commander push left
	KEY_UP, //Commander push up
	KEY_RIGHT, //Commander push right
	KEY_DOWN, //Commadner push down
	KEY_N, //Commander rotate left
	KEY_M //Commadner rotate right
};

std::unordered_set<int> captured_keys_radio_audio = {
	KEY_G, //Steering wheel talk
	KEY_E, //Music button
	KEY_HOME, //Home button
	KEY_R, //Nav button
	KEY_BACKSPACE, //Back button
	KEY_T, //Fav button
	KEY_ENTER, //Commander depress
	KEY_LEFT, //Commander push left
	KEY_UP, //Commander push up
	KEY_RIGHT, //Commander push right
	KEY_DOWN, //Commadner push down
	KEY_N, //Commander rotate left
	KEY_M //Commadner rotate right
};

std::unordered_set <int> *captured_keys;

static void destroy_device(int fd)
{
    ::ioctl(fd, UI_DEV_DESTROY);
    ::close(fd);
}

static void __attribute__((destructor)) cleanup(void)
{
    if (outfd >= 0) {
        destroy_device(outfd);
    }
}

static void signal_handler(int)
{
    exit(1);
}

static int shouldIntercept = false;

static void should_intercept_enable(int)
{
	shouldIntercept = true;
	switch_audio(AUDIO_AA);
	captured_keys = &captured_keys_aa_audio;
}

static void should_intercept_disable(int)
{
	shouldIntercept = false;
	switch_audio(AUDIO_RADIO);
}

// SUPER HACKY: Create a bunch of dummy input devices to force our device to get created at event6
static int make_dummy(void)
{
    static int counter = 0;
    int dummy = ::open("/dev/uinput", O_WRONLY);
    ::ioctl(dummy, UI_SET_EVBIT, EV_SYN);

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof uidev);
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "dummy%d", counter++);
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0xfedc;
    uidev.id.version = 1;

    if (::write(dummy, &uidev, sizeof uidev) != sizeof uidev) {
        err(1, "failed to write uidev struct");
    }

    if (::ioctl(dummy, UI_DEV_CREATE) != 0) {
        err(1, "failed to create device");
    }

    return dummy;
}

static void initialize(void)
{
    signal(SIGINT, signal_handler);
    signal(SIGUSR1, should_intercept_enable);
    signal(SIGUSR2, should_intercept_disable);

    int dummies[3];
    dummies[0] = make_dummy();
    dummies[1] = make_dummy();
    dummies[2] = make_dummy();

    outfd = ::open("/dev/uinput", O_WRONLY);
    if (outfd < 0) {
        err(1, "failed to open uinput device");
    }

    infd = ::open(source_device, O_RDONLY);
    if (infd < 0) {
        err(1, "failed to open input device %s", source_device);
    }

    if (::ioctl(outfd, UI_SET_EVBIT, EV_SYN) != 0) {
        err(1, "failed to enable EV_SYN");
    }

    if (::ioctl(outfd, UI_SET_EVBIT, EV_KEY) != 0) {
        err(1, "failed to enable EV_SYN");
    }

    if (::ioctl(outfd, UI_SET_EVBIT, EV_LED) != 0) {
        err(1, "failed to enable EV_SYN");
    }

    for (int i = 0; i < 199; ++i) {
        if (::ioctl(outfd, UI_SET_KEYBIT, i) != 0) {
            err(1, "failed to enable key bit %d", i);
        }
    }

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof uidev);
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, filtered_name);
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0xfedc;
    uidev.id.version = 1;

    if (::write(outfd, &uidev, sizeof uidev) != sizeof uidev) {
        err(1, "failed to write uidev struct");
    }

    // Replace the old device
    struct stat st;
    if (::stat(new_device_path, &st) == 0) {
        errx(1, "new_device_path '%s' exists before creating input device", new_device_path);
    }

    if (::ioctl(outfd, UI_DEV_CREATE) != 0) {
        err(1, "failed to create device");
    }

    if (::stat(new_device_path, &st) != 0) {
        err(1, "couldn't stat created device");
    }

    // HACK: Wait for udev to populate /dev/input/
    sleep(1);

    if (::unlink(source_device) != 0) {
        err(1, "failed to unlink source device '%s'", source_device);
    }

    if (::rename(new_device_path, source_device) != 0) {
        err(1, "failed to move new device");
    }

    destroy_device(dummies[0]);
    destroy_device(dummies[1]);
    destroy_device(dummies[2]);
}

static void __attribute__((noreturn)) loop(void)
{
    enum class events {
        input,
    };

    int epfd = ::epoll_create(1);
    if (epfd < 0) {
        err(1, "failed to create epoll fd");
    }

    struct epoll_event event {
        .events = EPOLLIN,
        .data = {
            .u64 = uint64_t(events::input)
        }
    };

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, infd, &event) != 0) {
        err(1, "failed to add input fd to epoll set");
    }

    constexpr size_t buffer_size = 64;
    std::vector<struct iovec> iovecs;
    iovecs.reserve(buffer_size);
    while (epoll_wait(epfd, &event, 1, -1) >= 0 || errno == EINTR) {
		if (errno == EINTR)
		{
			errno = 0;
			continue;
		}
        switch (events(event.data.u64)) {
            case events::input:
            {
                iovecs.clear();
                struct input_event ev_buf[buffer_size];
                ssize_t bytes_read = ::read(infd, &ev_buf, sizeof ev_buf);
                if (bytes_read <= 0 || bytes_read % sizeof(struct input_event) != 0) {
                    err(1, "failed to read event");
                }

                ssize_t events_read = bytes_read / sizeof (struct input_event);
                for (int i = 0; i < events_read; ++i) {
                    struct input_event *ev = &ev_buf[i];
                    bool matched = false;
					if (shouldIntercept && ev->code == KEY_T)
					{
						if (ev->value == 1)
						{
							if (audio_status == AUDIO_AA)
							{
								switch_audio(AUDIO_RADIO);
								captured_keys = &captured_keys_radio_audio;
							}
							else
							{
								switch_audio(AUDIO_AA);
								captured_keys = &captured_keys_aa_audio;
							}
						}
						matched = true;
					}
					else if (shouldIntercept)
					{
						for (const auto &matcher : matchers) {
							if (matcher(ev)) {
								matched = true;
								break;
							}
						}
					}

                    if (!matched) {
                        iovecs.push_back({
                            .iov_base = ev,
                            .iov_len = sizeof (struct input_event)
                        });
                    }
                }

                ::writev(outfd, iovecs.data(), iovecs.size());
            }
        }
    }

    err(1, "epoll_wait failed");
}

int main(int argc, const char *argv[])
{
    // Try not to brick the car
    prevent_brick("/tmp/mnt/data/enable_input_filter");

    // BEGIN SCARY STUFF:
    initialize();

    DBusError error;
    dbus_error_init(&error);

    DBusConnection *hmi_bus = dbus_connection_open(HMI_BUS_ADDRESS, &error);
    if (!hmi_bus) {
        errx(1, "failed to connect to HMI bus: %s: %s\n", error.name, error.message);
    }

    if (!dbus_bus_register(hmi_bus, &error)) {
        errx(1, "failed to register with HMI bus: %s: %s\n", error.name, error.message);
    }

    matchers.push_back(
        [hmi_bus] (const struct input_event *ev) {
            if (ev->type == EV_KEY && captured_keys->count(ev->code)) {
                printf("received talk button, status = %d\n", ev->value);

                DBusMessage *msg = dbus_message_new_signal(
                    "/us/insolit/mazda/connector", "us.insolit.mazda.connector", "KeyEvent");

                if (!msg) {
                    errx(1, "failed to create dbus message");
                }

                DBusMessageIter iter;
                dbus_message_iter_init_append(msg, &iter);

                if (!dbus_message_encode_input_event(&iter, ev)) {
                    errx(1, "failed to append input event");
                }

                if (!dbus_connection_send(hmi_bus, msg, nullptr)) {
                    errx(1, "failed to send dbus message");
                }

                dbus_connection_flush(hmi_bus);
                dbus_message_unref(msg);

                return true;
            }

            return false;
        });

    loop();
}
