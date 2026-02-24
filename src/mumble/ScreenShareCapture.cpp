// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ScreenShareCapture.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QDebug>
#include <QImage>
#include <QMutexLocker>

#include <pipewire/keys.h>
#include <pipewire/stream.h>

#include <spa/buffer/buffer.h>
#include <spa/param/format.h>
#include <spa/pod/builder.h>
#include <spa/pod/pod.h>
#include <spa/utils/defs.h>

#include <QLibrary>
#include <fcntl.h>
#include <unistd.h>

// Video format enum values needed for SPA format negotiation.
// These are stable ABI values from spa/param/video/format.h which
// is not bundled in our 3rdparty headers.
enum spa_video_format_subset {
	SPA_VIDEO_FORMAT_UNKNOWN = 0,
	SPA_VIDEO_FORMAT_BGRx    = 20,
	SPA_VIDEO_FORMAT_RGBx    = 4,
	SPA_VIDEO_FORMAT_RGBA    = 5,
	SPA_VIDEO_FORMAT_BGRA    = 21,
	SPA_VIDEO_FORMAT_RGB     = 27,
	SPA_VIDEO_FORMAT_BGR     = 28,
};

// PipeWire stream flags
#ifndef PW_STREAM_FLAG_AUTOCONNECT
#	define PW_STREAM_FLAG_AUTOCONNECT (1 << 0)
#endif
#ifndef PW_STREAM_FLAG_MAP_BUFFERS
#	define PW_STREAM_FLAG_MAP_BUFFERS (1 << 2)
#endif

// PipeWire direction
#ifndef PW_DIRECTION_INPUT
#	define PW_DIRECTION_INPUT 0
#endif

// SPA param enum format
#ifndef SPA_PARAM_EnumFormat
#	define SPA_PARAM_EnumFormat 3
#endif

static const QString PORTAL_SERVICE    = QStringLiteral("org.freedesktop.portal.Desktop");
static const QString PORTAL_PATH       = QStringLiteral("/org/freedesktop/portal/desktop");
static const QString PORTAL_SCREENCAST = QStringLiteral("org.freedesktop.portal.ScreenCast");
static const QString PORTAL_REQUEST_IF = QStringLiteral("org.freedesktop.portal.Request");

/// Build a D-Bus a{sv} argument from a QVariantMap.
/// D-Bus requires map values to be wrapped as variants (QDBusVariant).
static QVariant buildDBusOptions(const QVariantMap &options) {
	QDBusArgument arg;
	arg.beginMap(QMetaType::fromType< QString >(), QMetaType::fromType< QDBusVariant >());
	for (auto it = options.constBegin(); it != options.constEnd(); ++it) {
		arg.beginMapEntry();
		arg << it.key();
		arg << QDBusVariant(it.value());
		arg.endMapEntry();
	}
	arg.endMap();
	return QVariant::fromValue(arg);
}

/// Extract the Response signal arguments (uint response, a{sv} results) from a raw QDBusMessage.
/// This avoids Qt's automatic a{sv} → QVariantMap demarshalling which crashes on some Qt6 versions.
static bool parsePortalResponse(const QDBusMessage &msg, uint &response, QVariantMap &results) {
	QList< QVariant > args = msg.arguments();
	if (args.size() < 2) {
		return false;
	}

	response = args.at(0).toUInt();

	// The second argument is a{sv} which Qt delivers as QDBusArgument.
	// We must read values as QDBusVariant to avoid Qt6 recursively demarshalling
	// complex nested types like a(ua{sv}) which triggers a crash.
	const QVariant &resultsVar = args.at(1);
	if (resultsVar.canConvert< QDBusArgument >()) {
		const QDBusArgument dbusArg = resultsVar.value< QDBusArgument >();
		dbusArg.beginMap();
		while (!dbusArg.atEnd()) {
			QString key;
			QDBusVariant val;
			dbusArg.beginMapEntry();
			dbusArg >> key >> val;
			dbusArg.endMapEntry();
			results.insert(key, val.variant());
		}
		dbusArg.endMap();
	} else if (resultsVar.canConvert< QVariantMap >()) {
		results = resultsVar.toMap();
	}

	return true;
}

// PipeWire function pointers (loaded dynamically, same pattern as PipeWire.cpp)
struct PipeWireFuncs {
	QLibrary lib;
	bool ok = false;

	void (*pw_init)(int *argc, char **argv[])                                                       = nullptr;
	pw_loop *(*pw_loop_new)(const spa_dict *props)                                                  = nullptr;
	void (*pw_loop_destroy)(pw_loop *loop)                                                          = nullptr;
	pw_thread_loop *(*pw_thread_loop_new_full)(pw_loop *loop, const char *name, const spa_dict *props) = nullptr;
	void (*pw_thread_loop_destroy)(pw_thread_loop *loop)                                            = nullptr;
	int (*pw_thread_loop_start)(pw_thread_loop *loop)                                               = nullptr;
	int (*pw_thread_loop_stop)(pw_thread_loop *loop)                                                = nullptr;
	void (*pw_thread_loop_lock)(pw_thread_loop *loop)                                               = nullptr;
	void (*pw_thread_loop_unlock)(pw_thread_loop *loop)                                             = nullptr;
	pw_properties *(*pw_properties_new)(const char *key, ...)                                       = nullptr;
	pw_stream *(*pw_stream_new_simple)(pw_loop *loop, const char *name, pw_properties *props,
									   const pw_stream_events *events, void *data)                  = nullptr;
	void (*pw_stream_destroy)(pw_stream *stream)                                                    = nullptr;
	int (*pw_stream_connect)(pw_stream *stream, uint32_t direction, uint32_t target_id, uint32_t flags,
							 const spa_pod **params, uint32_t n_params)                             = nullptr;
	pw_buffer *(*pw_stream_dequeue_buffer)(pw_stream *stream)                                       = nullptr;
	int (*pw_stream_queue_buffer)(pw_stream *stream, pw_buffer *buffer)                             = nullptr;

	bool load() {
		const QStringList names{ "libpipewire.so", "libpipewire-0.3.so", "libpipewire-0.3.so.0" };
		for (const auto &name : names) {
			lib.setFileName(name);
			if (lib.load())
				break;
		}
		if (!lib.isLoaded())
			return false;

#define RESOLVE_PW(var)                                                  \
	var = reinterpret_cast< decltype(var) >(lib.resolve(#var)); \
	if (!var)                                                     \
		return false;

		RESOLVE_PW(pw_init);
		RESOLVE_PW(pw_loop_new);
		RESOLVE_PW(pw_loop_destroy);
		RESOLVE_PW(pw_thread_loop_new_full);
		RESOLVE_PW(pw_thread_loop_destroy);
		RESOLVE_PW(pw_thread_loop_start);
		RESOLVE_PW(pw_thread_loop_stop);
		RESOLVE_PW(pw_thread_loop_lock);
		RESOLVE_PW(pw_thread_loop_unlock);
		RESOLVE_PW(pw_properties_new);
		RESOLVE_PW(pw_stream_new_simple);
		RESOLVE_PW(pw_stream_destroy);
		RESOLVE_PW(pw_stream_connect);
		RESOLVE_PW(pw_stream_dequeue_buffer);
		RESOLVE_PW(pw_stream_queue_buffer);

#undef RESOLVE_PW

		pw_init(nullptr, nullptr);
		ok = true;
		return true;
	}
};

static PipeWireFuncs s_pw;
static bool s_pwInitialized = false;

static bool ensurePipeWire() {
	if (s_pwInitialized)
		return s_pw.ok;
	s_pwInitialized = true;
	return s_pw.load();
}

ScreenShareCapture::ScreenShareCapture(int targetWidth, int targetHeight, int fps, QObject *parent)
	: QObject(parent), m_targetWidth(targetWidth), m_targetHeight(targetHeight), m_fps(fps) {
	connect(&m_frameTimer, &QTimer::timeout, this, &ScreenShareCapture::onFrameTimer);
}

ScreenShareCapture::~ScreenShareCapture() {
	stop();
}

bool ScreenShareCapture::isCapturing() const {
	return m_capturing;
}

void ScreenShareCapture::start() {
	qWarning("ScreenShareCapture::start() called");
	if (m_capturing) {
		return;
	}

	if (!ensurePipeWire()) {
		emit captureError(tr("Failed to load PipeWire library"));
		return;
	}

	// Sanitize D-Bus sender name for use in request handle path
	m_senderName = QDBusConnection::sessionBus().baseService();
	m_senderName.remove(':');
	m_senderName.replace('.', '_');
	qWarning("ScreenShareCapture: sender=%s, calling createSession", qPrintable(m_senderName));

	createSession();
}

void ScreenShareCapture::stop() {
	if (!m_capturing) {
		return;
	}

	m_frameTimer.stop();
	cleanupPipeWire();

	// Close portal session if we have one
	if (!m_sessionHandle.isEmpty()) {
		QDBusMessage msg = QDBusMessage::createMethodCall(PORTAL_SERVICE, m_sessionHandle,
														  QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Close"));
		QDBusConnection::sessionBus().asyncCall(msg);
		m_sessionHandle.clear();
	}

	m_capturing = false;
	emit captureStopped();
}

void ScreenShareCapture::createSession() {
	++m_requestCounter;
	QString token       = QStringLiteral("mumble_screenshare_%1").arg(m_requestCounter);
	QString requestPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(m_senderName, token);

	// Subscribe to the Response signal before making the call.
	// Use QDBusMessage slot to avoid Qt6 automatic a{sv} demarshalling crash.
	QDBusConnection::sessionBus().connect(PORTAL_SERVICE, requestPath, PORTAL_REQUEST_IF, QStringLiteral("Response"), this,
										  SLOT(onCreateSessionResponse(QDBusMessage)));

	QDBusMessage msg =
		QDBusMessage::createMethodCall(PORTAL_SERVICE, PORTAL_PATH, PORTAL_SCREENCAST, QStringLiteral("CreateSession"));

	QVariantMap options;
	options[QStringLiteral("handle_token")]         = token;
	options[QStringLiteral("session_handle_token")] = QStringLiteral("mumble_session_%1").arg(m_requestCounter);

	msg << buildDBusOptions(options);
	qWarning("ScreenShareCapture: sending CreateSession");
	QDBusConnection::sessionBus().asyncCall(msg);
}

void ScreenShareCapture::onCreateSessionResponse(const QDBusMessage &msg) {
	uint response = 0;
	QVariantMap results;
	if (!parsePortalResponse(msg, response, results)) {
		emit captureError(tr("Failed to parse CreateSession response"));
		return;
	}

	qWarning("ScreenShareCapture: CreateSession response=%u", response);
	if (response != 0) {
		emit captureError(tr("Portal CreateSession failed with response %1").arg(response));
		return;
	}

	m_sessionHandle = results.value("session_handle").toString();
	if (m_sessionHandle.isEmpty()) {
		emit captureError(tr("Portal returned empty session handle"));
		return;
	}

	qWarning("ScreenShareCapture: session_handle=%s", qPrintable(m_sessionHandle));
	selectSources();
}

void ScreenShareCapture::selectSources() {
	++m_requestCounter;
	QString token       = QStringLiteral("mumble_screenshare_%1").arg(m_requestCounter);
	QString requestPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(m_senderName, token);

	QDBusConnection::sessionBus().connect(PORTAL_SERVICE, requestPath, PORTAL_REQUEST_IF, QStringLiteral("Response"), this,
										  SLOT(onSelectSourcesResponse(QDBusMessage)));

	QDBusMessage msg =
		QDBusMessage::createMethodCall(PORTAL_SERVICE, PORTAL_PATH, PORTAL_SCREENCAST, QStringLiteral("SelectSources"));

	QVariantMap options;
	options[QStringLiteral("handle_token")] = token;
	options[QStringLiteral("types")]        = QVariant::fromValue(static_cast< uint >(3)); // MONITOR | WINDOW
	options[QStringLiteral("multiple")]     = false;

	msg << QVariant::fromValue(QDBusObjectPath(m_sessionHandle)) << buildDBusOptions(options);
	QDBusConnection::sessionBus().asyncCall(msg);
}

void ScreenShareCapture::onSelectSourcesResponse(const QDBusMessage &msg) {
	uint response = 0;
	QVariantMap results;
	if (!parsePortalResponse(msg, response, results)) {
		emit captureError(tr("Failed to parse SelectSources response"));
		return;
	}

	qWarning("ScreenShareCapture: SelectSources response=%u", response);
	if (response != 0) {
		emit captureError(tr("Portal SelectSources failed with response %1").arg(response));
		return;
	}

	startPortal();
}

void ScreenShareCapture::startPortal() {
	++m_requestCounter;
	QString token       = QStringLiteral("mumble_screenshare_%1").arg(m_requestCounter);
	QString requestPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(m_senderName, token);

	QDBusConnection::sessionBus().connect(PORTAL_SERVICE, requestPath, PORTAL_REQUEST_IF, QStringLiteral("Response"), this,
										  SLOT(onStartResponse(QDBusMessage)));

	QDBusMessage msg =
		QDBusMessage::createMethodCall(PORTAL_SERVICE, PORTAL_PATH, PORTAL_SCREENCAST, QStringLiteral("Start"));

	QVariantMap options;
	options[QStringLiteral("handle_token")] = token;

	msg << QVariant::fromValue(QDBusObjectPath(m_sessionHandle)) << QString() << buildDBusOptions(options);
	QDBusConnection::sessionBus().asyncCall(msg);
}

void ScreenShareCapture::onStartResponse(const QDBusMessage &msg) {
	// Parse the Start response entirely from raw D-Bus arguments to avoid
	// Qt6's broken a{sv} → QVariantMap demarshalling at every level.
	// Response signal is (u response, a{sv} results) where results contains
	// "streams" → a(ua{sv}).
	QList< QVariant > args = msg.arguments();
	if (args.size() < 2) {
		emit captureError(tr("Failed to parse Start response"));
		return;
	}

	uint response = args.at(0).toUInt();
	qWarning("ScreenShareCapture: Start response=%u", response);
	if (response != 0) {
		emit captureError(tr("Portal Start failed with response %1").arg(response));
		return;
	}

	// Read the a{sv} results dict directly from msg args
	const QDBusArgument resultsArg = args.at(1).value< QDBusArgument >();
	uint32_t nodeId                = 0;
	bool found                     = false;

	resultsArg.beginMap();
	while (!resultsArg.atEnd()) {
		QString key;
		QDBusVariant val;
		resultsArg.beginMapEntry();
		resultsArg >> key >> val;
		resultsArg.endMapEntry();

		if (key == QStringLiteral("streams")) {
			// val.variant() contains a(ua{sv}) as QDBusArgument
			const QDBusArgument streamsArg = val.variant().value< QDBusArgument >();
			qWarning("ScreenShareCapture: parsing streams, sig=%s",
					 qPrintable(streamsArg.currentSignature()));
			streamsArg.beginArray();
			while (!streamsArg.atEnd()) {
				qWarning("ScreenShareCapture: reading stream struct");
				streamsArg.beginStructure();
				qWarning("ScreenShareCapture: reading nodeId, sig=%s",
						 qPrintable(streamsArg.currentSignature()));
				streamsArg >> nodeId;
				qWarning("ScreenShareCapture: nodeId=%u, skipping props", nodeId);
				// Read the inner a{sv} as QVariant (keeps it lazy/wrapped)
				QVariant propsSkip;
				streamsArg >> propsSkip;
				qWarning("ScreenShareCapture: props skipped");
				streamsArg.endStructure();
				found = true;
				break;
			}
			streamsArg.endArray();
		}
	}
	resultsArg.endMap();

	qWarning("ScreenShareCapture: streams parsed, nodeId=%u found=%d", nodeId, found);

	if (!found) {
		emit captureError(tr("No streams found in portal response"));
		return;
	}

	// Now open the PipeWire remote FD
	QDBusMessage fdMsg =
		QDBusMessage::createMethodCall(PORTAL_SERVICE, PORTAL_PATH, PORTAL_SCREENCAST, QStringLiteral("OpenPipeWireRemote"));
	fdMsg << QVariant::fromValue(QDBusObjectPath(m_sessionHandle)) << buildDBusOptions(QVariantMap());

	QDBusReply< QDBusUnixFileDescriptor > fdReply = QDBusConnection::sessionBus().call(fdMsg);
	if (!fdReply.isValid()) {
		emit captureError(tr("Failed to get PipeWire FD: %1").arg(fdReply.error().message()));
		return;
	}

	m_pwFd = ::dup(fdReply.value().fileDescriptor());
	if (m_pwFd < 0) {
		emit captureError(tr("Failed to duplicate PipeWire FD"));
		return;
	}

	if (!setupPipeWireStream(nodeId)) {
		emit captureError(tr("Failed to set up PipeWire stream"));
		return;
	}

	m_capturing = true;
	m_frameTimer.start(1000 / m_fps);
	emit captureStarted();
}

bool ScreenShareCapture::setupPipeWireStream(uint32_t nodeId) {
	m_pwLoop = s_pw.pw_loop_new(nullptr);
	if (!m_pwLoop) {
		qWarning("ScreenShareCapture: Failed to create PipeWire loop");
		return false;
	}

	pw_properties *props = s_pw.pw_properties_new(PW_KEY_APP_NAME, "Mumble", PW_KEY_MEDIA_TYPE, "Video",
												   PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE,
												   "Communication", nullptr);

	m_pwEvents          = new pw_stream_events();
	m_pwEvents->version = PW_VERSION_STREAM_EVENTS;
	m_pwEvents->process = &ScreenShareCapture::onStreamProcess;

	m_pwStream = s_pw.pw_stream_new_simple(m_pwLoop, "Mumble ScreenShare", props, m_pwEvents, this);
	if (!m_pwStream) {
		qWarning("ScreenShareCapture: Failed to create PipeWire stream");
		return false;
	}

	// Build the video format parameter.
	// We accept BGRx and RGBx formats (common for screen capture on Wayland).
	uint8_t buffer[1024];
	spa_pod_builder builder{};
	builder.data = buffer;
	builder.size = sizeof(buffer);

	// Build a simple format pod: Video/raw with BGRx format
	// We use the low-level pod builder since we don't have the spa video format-utils header.
	spa_pod_frame frame;
	spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);

	spa_pod_builder_add(&builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
						SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);

	// Accept multiple video formats
	spa_pod_builder_add(
		&builder, SPA_FORMAT_VIDEO_format,
		SPA_POD_CHOICE_ENUM_Id(7, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx,
							   SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGB,
							   SPA_VIDEO_FORMAT_BGR),
		0);

	spa_pod *formatPod = static_cast< spa_pod * >(spa_pod_builder_pop(&builder, &frame));

	const spa_pod *params[] = { formatPod };

	int ret = s_pw.pw_stream_connect(m_pwStream, PW_DIRECTION_INPUT, nodeId,
									 PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
	if (ret < 0) {
		qWarning("ScreenShareCapture: pw_stream_connect failed: %d", ret);
		return false;
	}

	m_pwThread = s_pw.pw_thread_loop_new_full(m_pwLoop, "Mumble ScreenShare", nullptr);
	if (!m_pwThread) {
		qWarning("ScreenShareCapture: Failed to create PipeWire thread loop");
		return false;
	}

	s_pw.pw_thread_loop_start(m_pwThread);
	return true;
}

void ScreenShareCapture::onStreamProcess(void *userdata) {
	auto *self = static_cast< ScreenShareCapture * >(userdata);

	pw_buffer *pwBuf = s_pw.pw_stream_dequeue_buffer(self->m_pwStream);
	if (!pwBuf) {
		return;
	}

	spa_buffer *spaBuf = pwBuf->buffer;
	spa_data &data     = spaBuf->datas[0];

	if (!data.data || data.chunk->size == 0) {
		s_pw.pw_stream_queue_buffer(self->m_pwStream, pwBuf);
		return;
	}

	// Determine dimensions from chunk metadata.
	// The stride tells us bytes per row, and size / stride gives rows.
	const int stride = data.chunk->stride;
	const int height = (stride > 0) ? static_cast< int >(data.chunk->size) / stride : 0;
	const int bpp    = 4; // BGRx/RGBx = 4 bytes per pixel
	const int width  = (stride > 0) ? stride / bpp : 0;

	constexpr int MAX_DIMENSION = 8192;
	if (width <= 0 || height <= 0 || width > MAX_DIMENSION || height > MAX_DIMENSION) {
		s_pw.pw_stream_queue_buffer(self->m_pwStream, pwBuf);
		return;
	}

	// Convert to RGB. We assume BGRx format (most common from screen capture).
	const uint8_t *src = static_cast< const uint8_t * >(data.data);

	int outW = width;
	int outH = height;

	// Scale to target resolution if source is larger
	bool needScale = (width > self->m_targetWidth || height > self->m_targetHeight);

	// .copy() to own the pixel data before we return the PipeWire buffer
	QImage srcImage = QImage(src, width, height, stride, QImage::Format_RGB32).copy();
	QImage finalImage;

	if (needScale) {
		finalImage =
			srcImage.scaled(self->m_targetWidth, self->m_targetHeight, Qt::KeepAspectRatio, Qt::FastTransformation)
				.convertToFormat(QImage::Format_RGB888);
	} else {
		finalImage = srcImage.convertToFormat(QImage::Format_RGB888);
	}

	outW = finalImage.width();
	outH = finalImage.height();

	// Ensure dimensions are even (required by VP8 encoder)
	outW = outW & ~1;
	outH = outH & ~1;
	if (outW != finalImage.width() || outH != finalImage.height()) {
		finalImage = finalImage.copy(0, 0, outW, outH);
	}

	QByteArray rgbData(reinterpret_cast< const char * >(finalImage.constBits()), finalImage.sizeInBytes());

	{
		QMutexLocker lock(&self->m_frameMutex);
		self->m_latestFrame       = rgbData;
		self->m_latestFrameWidth  = outW;
		self->m_latestFrameHeight = outH;
		self->m_newFrameAvailable = true;
	}

	s_pw.pw_stream_queue_buffer(self->m_pwStream, pwBuf);
}

void ScreenShareCapture::onFrameTimer() {
	QByteArray frame;
	int w = 0, h = 0;

	{
		QMutexLocker lock(&m_frameMutex);
		if (!m_newFrameAvailable) {
			return;
		}
		frame             = m_latestFrame;
		w                 = m_latestFrameWidth;
		h                 = m_latestFrameHeight;
		m_newFrameAvailable = false;
	}

	emit frameCaptured(frame, w, h);
}

void ScreenShareCapture::cleanupPipeWire() {
	// Stop the thread loop first so no callbacks fire during cleanup
	if (m_pwThread) {
		s_pw.pw_thread_loop_stop(m_pwThread);
	}

	// Destroy the stream (safe now that the thread loop is stopped)
	if (m_pwStream) {
		s_pw.pw_stream_destroy(m_pwStream);
		m_pwStream = nullptr;
	}

	// Destroy the thread loop
	if (m_pwThread) {
		s_pw.pw_thread_loop_destroy(m_pwThread);
		m_pwThread = nullptr;
	}

	// Destroy the main loop
	if (m_pwLoop) {
		s_pw.pw_loop_destroy(m_pwLoop);
		m_pwLoop = nullptr;
	}

	// Now safe to delete events struct (no callbacks can fire)
	if (m_pwEvents) {
		delete m_pwEvents;
		m_pwEvents = nullptr;
	}

	if (m_pwFd >= 0) {
		::close(m_pwFd);
		m_pwFd = -1;
	}
}
