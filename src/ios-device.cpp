/**
 * node-ios-device
 * Copyright (c) 2013-2016 by Appcelerator, Inc. All Rights Reserved.
 * Licensed under the terms of the Apache Public License
 * Please see the LICENSE included with this distribution for details.
 */

#include <nan.h>
#include <node.h>
#include <v8.h>
#include <stdlib.h>
#include <string>
#include <map>
#include <thread>
#include <boost/format.hpp>
#include <boost/thread.hpp>
#include "mobiledevice.h"

using namespace v8;

/**
 * A struct to track listener properties such as the JavaScript callback
 * function.
 */
typedef struct Listener {
	Nan::Persistent<Function> callback;
} Listener;

/**
 * Globals
 */
static CFMutableDictionaryRef connectedDevices;
static Nan::Persistent<Object> emitter;

static std::condition_variable pendingCond;
static std::mutex pendingMutex;
static int pendingEvents = 0;

static bool devicesChanged = false;
static boost::shared_mutex deviceMutex;

/**
 * Emits a debug message.
 */
static void debug(const std::string& output) {
	Local<Object> ee = Nan::New(emitter);
	if (!ee.IsEmpty()) {
		Local<Function> emit = Local<Function>::Cast(ee->Get(Nan::New("emit").ToLocalChecked()));
		Local<Value> args[2] = {
			Nan::New("debug").ToLocalChecked(),
			Nan::New(output.c_str()).ToLocalChecked()
		};
		emit->Call(ee, 2, args);
	}
}

/**
 * Formats and emits a debug message.
 */
static void debug(const char* format, ...) {
	int final_n;
	int n = strlen(format) * 2;
	std::string str;
	std::unique_ptr<char[]> formatted;
	va_list ap;
	while(1) {
		formatted.reset(new char[n]);
		strcpy(&formatted[0], format);
		va_start(ap, format);
		final_n = vsnprintf(&formatted[0], n, format, ap);
		va_end(ap);
		if (final_n < 0 || final_n >= n) {
			n += abs(final_n - n + 1);
		} else {
			break;
		}
	}
	debug(std::string(formatted.get()));
}

/**
 * Converts CFStringRef strings to C strings.
 */
static char* cfstring_to_cstr(CFStringRef str) {
	if (str != NULL) {
		// add 1 to make sure there's enough buffer for the utf-8 string and the null character
		CFIndex length = CFStringGetLength(str) + 1;
		CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8);
		char* buffer = (char*)malloc(maxSize);
		if (CFStringGetCString(str, buffer, maxSize, kCFStringEncodingUTF8)) {
			return buffer;
		}
	}
	return NULL;
}

/**
 * Device object that persists while the device is plugged in. It contains the
 * original MobileDevice device reference and a V8 JavaScript object containing
 * the devices properties.
 */
class Device {
public:
	am_device handle;
	std::map<std::string, std::string> props;
	int connected;
	CFStringRef udid;
	bool hostConnected;
	service_conn_t logConnection;
	CFSocketRef logSocket;
	CFRunLoopSourceRef logSource;
	Listener* logCallback;

	/**
	 * Constructs the device object.
	 */
	Device(am_device& dev) : handle(dev), connected(0), hostConnected(false), logSocket(NULL), logSource(NULL), logCallback(NULL) {
		this->udid = AMDeviceCopyDeviceIdentifier(dev);

		char* str = cfstring_to_cstr(this->udid);
		if (str != NULL) {
			this->props["udid"] = std::string(str);
			free(str);
		}
	}

	/**
	 * Disconnects and cleans up allocated memory.
	 */
	~Device() {
		this->disconnect(true);

		if (this->logCallback) {
			delete this->logCallback;
		}
		if (this->logSource) {
			CFRelease(this->logSource);
		}
		if (this->logSocket) {
			CFRelease(this->logSocket);
		}
	}

	/**
	 * Connects to the device, pairs with it, and starts a session. We use a
	 * connected counter so that we don't connect more than once.
	 */
	void connect() {
		if (this->connected++ > 0) {
			// already connected
			return;
		}

		// connect to the device
		mach_error_t rval = AMDeviceConnect(this->handle);
		if (rval == MDERR_SYSCALL) {
			throw std::runtime_error("Failed to connect to device: setsockopt() failed");
		} else if (rval == MDERR_QUERY_FAILED) {
			throw std::runtime_error("Failed to connect to device: the daemon query failed");
		} else if (rval == MDERR_INVALID_ARGUMENT) {
			throw std::runtime_error("Failed to connect to device: invalid argument, USBMuxConnectByPort returned 0xffffffff");
		} else if (rval != MDERR_OK) {
			throw std::runtime_error((boost::format("Failed to connect to device (0x%x)") % rval).str());
		}

		// if we're not paired, go ahead and pair now
		if (AMDeviceIsPaired(this->handle) != 1 && AMDevicePair(this->handle) != 1) {
			throw std::runtime_error("Failed to pair device");
		}

		// double check the pairing
		rval = AMDeviceValidatePairing(this->handle);
		if (rval == MDERR_INVALID_ARGUMENT) {
			throw std::runtime_error("Device is not paired: the device is null");
		} else if (rval == MDERR_DICT_NOT_LOADED) {
			throw std::runtime_error("Device is not paired: load_dict() failed");
		} else if (rval != MDERR_OK) {
			throw std::runtime_error((boost::format("Device is not paired (0x%x)") % rval).str());
		}

		// start the session
		rval = AMDeviceStartSession(this->handle);
		if (rval == MDERR_INVALID_ARGUMENT) {
			throw std::runtime_error("Failed to start session: the lockdown connection has not been established");
		} else if (rval == MDERR_DICT_NOT_LOADED) {
			throw std::runtime_error("Failed to start session: load_dict() failed");
		} else if (rval != MDERR_OK) {
			throw std::runtime_error((boost::format("Failed to start session (0x%x)") % rval).str());
		}
	}

	/**
	 * Disconnects the device if there are no other active connections to this
	 * device. Generally, force should not be set. It's mainly there for the
	 * destructor.
	 */
	void disconnect(const bool force = false) {
		if (force || --this->connected <= 0) {
			this->connected = 0;
			AMDeviceStopSession(this->handle);
			AMDeviceDisconnect(this->handle);
		}
	}

	/**
	 * Starts a service.
	 *
	 * Note that if the call to AMDeviceStartService() fails, it's probably
	 * because MobileDevice thinks we're connected and paired, but we're not.
	 */
	void startService(const char* serviceName, service_conn_t* conn) const {
		mach_error_t rval = AMDeviceStartService(this->handle, CFStringCreateWithCStringNoCopy(NULL, serviceName, kCFStringEncodingUTF8, NULL), conn, NULL);
		if (rval == MDERR_SYSCALL) {
			throw std::runtime_error((boost::format("Failed to start \"%s\" service due to system call error (0x%x)") % serviceName % rval).str());
		} else if (rval == MDERR_INVALID_ARGUMENT) {
			throw std::runtime_error((boost::format("Failed to start \"%s\" service due to invalid argument (0x%x)") % serviceName % rval).str());
		} else if (rval != MDERR_OK) {
			throw std::runtime_error((boost::format("Failed to start \"%s\" service (0x%x)") % serviceName % rval).str());
		}
	}

	/**
	 * Sets a property.
	 */
	void set(const char* key, CFStringRef id) {
		CFStringRef valueStr = (CFStringRef)AMDeviceCopyValue(this->handle, 0, id);
		if (valueStr != NULL) {
			char* value = cfstring_to_cstr(valueStr);
			CFRelease(valueStr);
			if (value != NULL) {
				this->props[key] = std::string(value);
				free(value);
			}
		}
	}
};

/**
 * Fetches additional info about a device for the given udid. Note that this
 * function runs in a thread so that the main thread's event loop isn't blocked.
 */
static void getDeviceInfo(Device* device) {
	boost::upgrade_lock<boost::shared_mutex> lock(deviceMutex);
	boost::upgrade_to_unique_lock<boost::shared_mutex> unique_lock(lock);

	// we must always set this flag
	devicesChanged = true;

	// if we already have the device info, don't get it again
	if (!CFDictionaryContainsKey(connectedDevices, device->udid)) {
		CFDictionarySetValue(connectedDevices, device->udid, device);
	}

	try {
		// connect to the device and get its information
		//printf("Connecting to device %s\n", device->props["udid"].c_str());
		device->connect();
		device->set("name",            CFSTR("DeviceName"));
		device->set("buildVersion",    CFSTR("BuildVersion"));
		device->set("cpuArchitecture", CFSTR("CPUArchitecture"));
		device->set("deviceClass",     CFSTR("DeviceClass"));
		device->set("deviceColor",     CFSTR("DeviceColor"));
		device->set("hardwareModel",   CFSTR("HardwareModel"));
		device->set("modelNumber",     CFSTR("ModelNumber"));
		device->set("productType",     CFSTR("ProductType"));
		device->set("productVersion",  CFSTR("ProductVersion"));
		device->set("serialNumber",    CFSTR("SerialNumber"));

		CFNumberRef valueNum = (CFNumberRef)AMDeviceCopyValue(device->handle, 0, CFSTR("HostAttached"));
		int64_t value = 0;
		CFNumberGetValue(valueNum, kCFNumberSInt64Type, &value);
		CFRelease(valueNum);
		device->hostConnected = value == 1;
	} catch (...) {
		//printf("Error!\n");
	}

	//printf("Disconnecting from device %s\n", device->props["udid"].c_str());
	device->disconnect();

	std::unique_lock<std::mutex> mlock(pendingMutex);
	if (pendingEvents > 0) {
		pendingEvents--;
	}
	if (pendingEvents == 0) {
		pendingCond.notify_one();
	}
}

/**
 * initEmitter()
 * Initializes the event emitter.
 */
NAN_METHOD(setEmitter) {
	if (info.Length() != 1) {
		return Nan::ThrowError(Exception::Error(Nan::New("Expected 1 argument").ToLocalChecked()));
	}

	if (!info[0]->IsObject()) {
		return Nan::ThrowError(Exception::Error(Nan::New("Argument \'emitter\' must be an object").ToLocalChecked()));
	}

	emitter.Reset(Local<Object>::Cast(info[0]));

	info.GetReturnValue().SetUndefined();
}

/**
 * pumpRunLoop()
 * Defines a JavaScript function that processes all pending notifications.
 */
NAN_METHOD(pump_run_loop) {
	// Note that this value is somewhat arbitrary. There was a report once that
	// this value was too low and the computer couldn't keep up.
	CFTimeInterval interval = 0.25;

	if (info.Length() > 0 && info[0]->IsNumber()) {
		Local<Number> intervalArg = Local<Number>::Cast(info[0]);
		interval = intervalArg->NumberValue();
	}

	// tick the run loop
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, interval, false);

	if (devicesChanged) {
		debug("Devices changed, emitting event");

		// immediately reset just in case we have a getDeviceInfo() call running
		// in the background
		devicesChanged = false;

		Local<Object> ee = Nan::New(emitter);
		if (!ee.IsEmpty()) {
			Local<Function> emit = Local<Function>::Cast(ee->Get(Nan::New("emit").ToLocalChecked()));
			Local<Value> args[1] = { Nan::New("devicesChanged").ToLocalChecked() };
			emit->Call(ee, 1, args);
		}
	}

	info.GetReturnValue().SetUndefined();
}

/**
 * devices()
 * Defines a JavaScript function that returns a JavaScript array of iOS devices.
 * This should be called after pumpRunLoop() has been called.
 */
NAN_METHOD(devices) {
	std::unique_lock<std::mutex> mlock(pendingMutex);
	if (pendingEvents > 0) {
		pendingCond.wait(mlock);
	}

	boost::shared_lock<boost::shared_mutex> lock(deviceMutex);

	Local<Array> result = Nan::New<Array>();

	CFIndex size = CFDictionaryGetCount(connectedDevices);
	Device** values = (Device**)malloc(size * sizeof(Device*));
	CFDictionaryGetKeysAndValues(connectedDevices, NULL, (const void **)values);

	debug("Found %d device%s", size, size == 1 ? "" : "s");
	for (CFIndex i = 0; i < size; i++) {
		Local<Object> p = Nan::New<Object>();
		for (std::map<std::string, std::string>::iterator it = values[i]->props.begin(); it != values[i]->props.end(); ++it) {
			Nan::Set(p, Nan::New(it->first).ToLocalChecked(), Nan::New(it->second).ToLocalChecked());
		}
		Nan::Set(result, i, p);
	}

	free(values);

	info.GetReturnValue().Set(result);
}

/**
 * The callback when a device notification is received.
 */
void on_device_notification(am_device_notification_callback_info* info, void* arg) {
	CFStringRef udid = AMDeviceCopyDeviceIdentifier(info->dev);
	bool exists = false;
	{
		boost::shared_lock<boost::shared_mutex> lock(deviceMutex);
		exists = CFDictionaryContainsKey(connectedDevices, udid);
	}

	if (!exists && info->msg == ADNCI_MSG_CONNECTED) {
		Device* device = new Device(info->dev);
		debug("Device connected, getting device info: %s", device->props["udid"].c_str());

		// tell devices() to wait until we've fetched the device info
		std::unique_lock<std::mutex> mlock(pendingMutex);
		if (pendingEvents < 0) {
			pendingEvents = 0;
		}
		pendingEvents++;

		std::thread(getDeviceInfo, device).detach();

	} else if (exists && info->msg == ADNCI_MSG_DISCONNECTED) {
		boost::upgrade_lock<boost::shared_mutex> lock(deviceMutex);
		boost::upgrade_to_unique_lock<boost::shared_mutex> unique_lock(lock);

		Device* device = (Device*)CFDictionaryGetValue(connectedDevices, udid);

		debug("Device disconnected: %s", device->props["udid"].c_str());

		// remove the device from the dictionary and destroy it
		CFDictionaryRemoveValue(connectedDevices, udid);

		if (device->logCallback) {
			delete device->logCallback;
		}
		if (device->logSource) {
			CFRelease(device->logSource);
		}
		if (device->logSocket) {
			CFRelease(device->logSocket);
		}

		delete device;
		devicesChanged = true;
	}
}

/**
 * installApp()
 * Defines a JavaScript function that installs an iOS app on the specified device.
 * This should be called after pumpRunLoop() has been called.
 */
NAN_METHOD(installApp) {
	char tmp[256];

	if (info.Length() < 2 || info[0]->IsUndefined() || info[1]->IsUndefined()) {
		return Nan::ThrowError(Exception::Error(Nan::New("Missing required arguments \'udid\' and \'appPath\'").ToLocalChecked()));
	}

	// validate the 'udid'
	if (!info[0]->IsString()) {
		return Nan::ThrowError(Exception::Error(Nan::New("Argument \'udid\' must be a string").ToLocalChecked()));
	}

	Handle<String> udidHandle = Handle<String>::Cast(info[0]);
	if (udidHandle->Length() == 0) {
		return Nan::ThrowError(Exception::Error(Nan::New("The \'udid\' must not be an empty string").ToLocalChecked()));
	}

	String::Utf8Value udidValue(udidHandle->ToString());
	char* udid = *udidValue;
	CFStringRef udidStr = CFStringCreateWithCString(NULL, (char*)*udidValue, kCFStringEncodingUTF8);

	if (!CFDictionaryContainsKey(connectedDevices, (const void*)udidStr)) {
		CFRelease(udidStr);
		snprintf(tmp, 256, "Device \'%s\' not connected", udid);
		return Nan::ThrowError(Exception::Error(Nan::New(tmp).ToLocalChecked()));
	}

	Device* deviceObj = (Device*)CFDictionaryGetValue(connectedDevices, udidStr);
	CFRelease(udidStr);
	am_device* device = &deviceObj->handle;

	// validate the 'appPath'
	if (!info[1]->IsString()) {
		return Nan::ThrowError(Exception::Error(Nan::New("Argument \'appPath\' must be a string").ToLocalChecked()));
	}

	Handle<String> appPathHandle = Handle<String>::Cast(info[1]);
	if (appPathHandle->Length() == 0) {
		return Nan::ThrowError(Exception::Error(Nan::New("The \'appPath\' must not be an empty string").ToLocalChecked()));
	}

	String::Utf8Value appPathValue(appPathHandle->ToString());
	char* appPath = *appPathValue;

	// check the file exists
	if (::access(appPath, F_OK) != 0) {
		snprintf(tmp, 256, "The app path \'%s\' does not exist", appPath);
		return Nan::ThrowError(Exception::Error(Nan::New(tmp).ToLocalChecked()));
	}

	// get the path to the app
	CFStringRef appPathStr = CFStringCreateWithCString(NULL, (char*)*appPathValue, kCFStringEncodingUTF8);
	CFURLRef relativeUrl = CFURLCreateWithFileSystemPath(NULL, appPathStr, kCFURLPOSIXPathStyle, false);
	CFURLRef localUrl = CFURLCopyAbsoluteURL(relativeUrl);
	CFRelease(appPathStr);
	CFRelease(relativeUrl);

	try {
		deviceObj->connect();
	} catch (std::runtime_error& e) {
		return Nan::ThrowError(Exception::Error(Nan::New(e.what()).ToLocalChecked()));
	}

	CFStringRef keys[] = { CFSTR("PackageType") };
	CFStringRef values[] = { CFSTR("Developer") };
	CFDictionaryRef options = CFDictionaryCreate(NULL, (const void **)&keys, (const void **)&values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	// copy .app to device
	mach_error_t rval = AMDeviceSecureTransferPath(0, *device, localUrl, options, NULL, 0);
	if (rval != MDERR_OK) {
		AMDeviceStopSession(*device);
		AMDeviceDisconnect(*device);
		deviceObj->connected--;
		CFRelease(options);
		CFRelease(localUrl);
		if (rval == -402653177) {
			return Nan::ThrowError(Exception::Error(Nan::New("Failed to copy app to device: can't install app that contains symlinks").ToLocalChecked()));
		} else {
			snprintf(tmp, 256, "Failed to copy app to device (0x%x)", rval);
			return Nan::ThrowError(Exception::Error(Nan::New(tmp).ToLocalChecked()));
		}
	}

	// install package on device
	rval = AMDeviceSecureInstallApplication(0, *device, localUrl, options, NULL, 0);
	if (rval != MDERR_OK) {
		AMDeviceStopSession(*device);
		AMDeviceDisconnect(*device);
		deviceObj->connected--;
		CFRelease(options);
		CFRelease(localUrl);
		if (rval == -402620395) {
			return Nan::ThrowError(Exception::Error(Nan::New("Failed to install app on device: most likely a provisioning profile issue").ToLocalChecked()));
		} else {
			snprintf(tmp, 256, "Failed to install app on device (0x%x)", rval);
			return Nan::ThrowError(Exception::Error(Nan::New(tmp).ToLocalChecked()));
		}
	}

	// cleanup
	deviceObj->disconnect();
	CFRelease(options);
	CFRelease(localUrl);

	info.GetReturnValue().SetUndefined();
}

/**
 * Handles new data from the socket when listening for a device's syslog messages.
 */
void LogSocketCallback(CFSocketRef s, CFSocketCallBackType type, CFDataRef address, const void *data, void *info) {
	Device* device = (Device*)info;
	Local<Function> callback = Nan::New<Function>(device->logCallback->callback);
	CFIndex length = CFDataGetLength((CFDataRef)data);
	const char *buffer = (const char*)CFDataGetBytePtr((CFDataRef)data);
	char* str = new char[length + 1];
	long i = 0;
	long j = 0;
	char c;
	Handle<Value> argv[1];

	while (length) {
		while (*buffer == '\0') {
			buffer++;
			length--;
			if (length == 0)
				return;
		}

		i = j = 0;

		while (i < length) {
			c = str[j] = buffer[i++];
			if (c == '\n' || c == '\0') {
				str[j] = '\0';
				if (j > 0) {
					argv[0] = Nan::New(str).ToLocalChecked();
					callback->Call(Nan::GetCurrentContext()->Global(), 1, argv);
				}
				j = 0;
				if (c == '\0') {
					break;
				}
			} else {
				++j;
			}
		}

		length -= i;
		buffer += i;
	}

	delete[] str;
}

/**
 * log()
 * Connects to the device and fires the callback with each line of output from
 * the device's syslog.
 */
NAN_METHOD(log) {
	char tmp[256];

	if (info.Length() < 2 || info[0]->IsUndefined() || info[1]->IsUndefined()) {
		return Nan::ThrowError(Exception::Error(Nan::New("Missing required arguments \'udid\' and \'appPath\'").ToLocalChecked()));
	}

	// validate the 'udid'
	if (!info[0]->IsString()) {
		return Nan::ThrowError(Exception::Error(Nan::New("Argument \'udid\' must be a string").ToLocalChecked()));
	}

	Handle<String> udidHandle = Handle<String>::Cast(info[0]);
	if (udidHandle->Length() == 0) {
		return Nan::ThrowError(Exception::Error(Nan::New("The \'udid\' must not be an empty string").ToLocalChecked()));
	}

	String::Utf8Value udidValue(udidHandle->ToString());
	char* udid = *udidValue;
	CFStringRef udidStr = CFStringCreateWithCString(NULL, (char*)*udidValue, kCFStringEncodingUTF8);

	if (!CFDictionaryContainsKey(connectedDevices, (const void*)udidStr)) {
		CFRelease(udidStr);
		snprintf(tmp, 256, "Device \'%s\' not connected", udid);
		return Nan::ThrowError(Exception::Error(Nan::New(tmp).ToLocalChecked()));
	}

	if (!info[1]->IsFunction()) {
		return Nan::ThrowError(Exception::Error(Nan::New("Argument \'callback\' must be a function").ToLocalChecked()));
	}

	Listener* logCallback = new Listener;
	logCallback->callback.Reset(Local<Function>::Cast(info[1]));

	Device* deviceObj = (Device*)CFDictionaryGetValue(connectedDevices, udidStr);
	CFRelease(udidStr);

	// It's possible for the iOS device to not be connected wirelessly instead
	// of with a cable. If that's the case, then AMDeviceStartService() will
	// fail to start the com.apple.syslog_relay service.
	if (!deviceObj->hostConnected) {
		return Nan::ThrowError(Exception::Error(Nan::New("iOS device must be connected to host").ToLocalChecked()));
	}

	service_conn_t connection;

	try {
		deviceObj->connect();
		deviceObj->startService(AMSVC_SYSLOG_RELAY, &connection);
	} catch (std::runtime_error& e) {
		return Nan::ThrowError(Exception::Error(Nan::New(e.what()).ToLocalChecked()));
	}

	deviceObj->disconnect();

	CFSocketContext socketCtx = { 0, deviceObj, NULL, NULL, NULL };
	CFSocketRef socket = CFSocketCreateWithNative(kCFAllocatorDefault, connection, kCFSocketDataCallBack, LogSocketCallback, &socketCtx);
	if (!socket) {
		return Nan::ThrowError(Exception::Error(Nan::New("Failed to create socket").ToLocalChecked()));
	}

	CFRunLoopSourceRef source = CFSocketCreateRunLoopSource(kCFAllocatorDefault, socket, 0);
	if (!source) {
		return Nan::ThrowError(Exception::Error(Nan::New("Failed to create socket run loop source").ToLocalChecked()));
	}

	CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);

	if (deviceObj->logCallback) {
		delete deviceObj->logCallback;
	}
	if (deviceObj->logSource) {
		CFRelease(deviceObj->logSource);
	}
	if (deviceObj->logSocket) {
		CFRelease(deviceObj->logSocket);
	}

	deviceObj->logConnection = connection;
	deviceObj->logSocket = socket;
	deviceObj->logSource = source;
	deviceObj->logCallback = logCallback;

	info.GetReturnValue().SetUndefined();
}

/**
 * Called when Node begins to shutdown so that we can clean up any allocated memory.
 */
static void cleanup(void *arg) {
	// free up connected devices
	CFIndex size = CFDictionaryGetCount(connectedDevices);
	CFStringRef* keys = (CFStringRef*)malloc(size * sizeof(CFStringRef));
	CFDictionaryGetKeysAndValues(connectedDevices, (const void **)keys, NULL);
	CFIndex i = 0;

	for (; i < size; i++) {
		Device* device = (Device*)CFDictionaryGetValue(connectedDevices, keys[i]);
		CFDictionaryRemoveValue(connectedDevices, keys[i]);
		delete device;
	}

	free(keys);
}

/**
 * Wire up the JavaScript functions, initialize the dictionaries, and subscribe
 * to the device notifications.
 */
static void init(Handle<Object> exports) {
	exports->Set(Nan::New("setEmitter").ToLocalChecked(),  Nan::New<FunctionTemplate>(setEmitter)->GetFunction());
	exports->Set(Nan::New("pumpRunLoop").ToLocalChecked(), Nan::New<FunctionTemplate>(pump_run_loop)->GetFunction());
	exports->Set(Nan::New("devices").ToLocalChecked(),     Nan::New<FunctionTemplate>(devices)->GetFunction());
	exports->Set(Nan::New("installApp").ToLocalChecked(),  Nan::New<FunctionTemplate>(installApp)->GetFunction());
	exports->Set(Nan::New("log").ToLocalChecked(),         Nan::New<FunctionTemplate>(log)->GetFunction());

	connectedDevices = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, NULL);

	am_device_notification notification;
	AMDeviceNotificationSubscribe(&on_device_notification, 0, 0, NULL, &notification);

	node::AtExit(cleanup);
}

NODE_MODULE(node_ios_device, init)
