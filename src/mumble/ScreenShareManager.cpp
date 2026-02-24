// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ScreenShareManager.h"

#include "ClientUser.h"
#include "Global.h"
#include "ServerHandler.h"
#include "ScreenShareCapture.h"
#include "ScreenShareDecoder.h"
#include "ScreenShareEncoder.h"
#include "ScreenShareViewer.h"

#include "Mumble.pb.h"

#include <QDateTime>
#include <QDebug>

ScreenShareManager::ScreenShareManager(QObject *parent) : QObject(parent) {
}

ScreenShareManager::~ScreenShareManager() {
	stopSharing();
	for (auto it = m_remoteStreams.begin(); it != m_remoteStreams.end(); ++it) {
		delete it->decoder;
		if (it->viewer) {
			it->viewer->disconnect(this);
			it->viewer->close();
		}
	}
	m_remoteStreams.clear();
}

bool ScreenShareManager::isSharing() const {
	return m_sharing;
}

void ScreenShareManager::startSharing() {
	qWarning("ScreenShareManager::startSharing() called");
	if (m_sharing) {
		return;
	}

	m_capture = new ScreenShareCapture(1280, 720, 15, this);
	m_encoder = new ScreenShareEncoder(1280, 720, 15, 1500, this);

	if (!m_encoder->init()) {
		qWarning("ScreenShareManager: Failed to init encoder");
		delete m_capture;
		delete m_encoder;
		m_capture = nullptr;
		m_encoder = nullptr;
		return;
	}
	qWarning("ScreenShareManager: encoder initialized OK");

	connect(m_capture, &ScreenShareCapture::captureStarted, this, &ScreenShareManager::onCaptureStarted);
	connect(m_capture, &ScreenShareCapture::captureStopped, this, &ScreenShareManager::onCaptureStopped);
	connect(m_capture, &ScreenShareCapture::captureError, this, &ScreenShareManager::onCaptureError);
	connect(m_capture, &ScreenShareCapture::frameCaptured, this, &ScreenShareManager::onFrameCaptured);
	connect(m_encoder, &ScreenShareEncoder::frameEncoded, this, &ScreenShareManager::onFrameEncoded);

	qWarning("ScreenShareManager: calling capture start");
	m_capture->start();
	qWarning("ScreenShareManager::startSharing() done");
}

void ScreenShareManager::stopSharing() {
	if (!m_sharing && !m_capture) {
		return;
	}

	if (m_capture) {
		m_capture->stop();
		delete m_capture;
		m_capture = nullptr;
	}

	if (m_encoder) {
		delete m_encoder;
		m_encoder = nullptr;
	}

	if (m_sharing) {
		// Notify the server that we stopped sharing
		MumbleProto::ScreenShare msg;
		msg.set_session(Global::get().uiSession);
		msg.set_active(false);
		Global::get().sh->sendMessage(msg);

		m_sharing = false;
		emit sharingStopped();
	}
}

void ScreenShareManager::onCaptureStarted() {
	m_sharing = true;

	// Notify the server that we started sharing
	MumbleProto::ScreenShare msg;
	msg.set_session(Global::get().uiSession);
	msg.set_active(true);
	msg.set_width(1280);
	msg.set_height(720);
	msg.set_fps(15);
	Global::get().sh->sendMessage(msg);

	emit sharingStarted();
}

void ScreenShareManager::onCaptureStopped() {
	// Capture stopped externally (e.g. portal closed)
	if (m_sharing) {
		MumbleProto::ScreenShare msg;
		msg.set_session(Global::get().uiSession);
		msg.set_active(false);
		Global::get().sh->sendMessage(msg);

		m_sharing = false;
		emit sharingStopped();
	}
}

void ScreenShareManager::onCaptureError(const QString &error) {
	qWarning("ScreenShareManager: Capture error: %s", qPrintable(error));
	stopSharing();
}

void ScreenShareManager::onFrameCaptured(QByteArray rgbFrame, int width, int height) {
	static int capturedCount = 0;
	if (capturedCount++ % 30 == 0) {
		qWarning("ScreenShareManager: onFrameCaptured #%d: %dx%d, %lld bytes", capturedCount, width, height, (long long)rgbFrame.size());
	}
	if (m_encoder) {
		m_encoder->encodeFrame(rgbFrame, width, height);
	}
}

void ScreenShareManager::onFrameEncoded(QByteArray encodedData, bool isKeyframe, uint64_t frameNumber) {
	static int encodedCount = 0;
	if (encodedCount++ % 30 == 0) {
		qWarning("ScreenShareManager: onFrameEncoded #%d: frame=%lu, %lld bytes, keyframe=%d",
				 encodedCount, (unsigned long)frameNumber, (long long)encodedData.size(), isKeyframe);
	}
	MumbleProto::ScreenShareFrame msg;
	msg.set_session(Global::get().uiSession);
	msg.set_frame_number(frameNumber);
	msg.set_frame_data(encodedData.constData(), static_cast< size_t >(encodedData.size()));
	msg.set_is_keyframe(isKeyframe);
	msg.set_width(1280);
	msg.set_height(720);
	msg.set_timestamp(static_cast< uint64_t >(QDateTime::currentMSecsSinceEpoch()));
	Global::get().sh->sendMessage(msg);
}

void ScreenShareManager::startViewing(ClientUser *user, uint32_t width, uint32_t height) {
	Q_UNUSED(width);
	Q_UNUSED(height);

	if (!user) {
		return;
	}

	uint32_t session = user->uiSession;

	// If already viewing this user, ignore
	if (m_remoteStreams.contains(session)) {
		return;
	}

	RemoteStream rs;
	rs.session = session;
	rs.decoder = new ScreenShareDecoder(this);

	if (!rs.decoder->init()) {
		qWarning("ScreenShareManager: Failed to init decoder for user %d", session);
		delete rs.decoder;
		return;
	}

	rs.viewer = new ScreenShareViewer(user->qsName);
	rs.viewer->show();

	connect(rs.decoder, &ScreenShareDecoder::frameDecoded, rs.viewer, &ScreenShareViewer::updateFrame);
	connect(rs.viewer, &ScreenShareViewer::viewerClosed, this, &ScreenShareManager::onViewerClosed);

	m_remoteStreams.insert(session, rs);
}

void ScreenShareManager::stopViewing(ClientUser *user) {
	if (!user) {
		return;
	}

	uint32_t session = user->uiSession;
	auto it          = m_remoteStreams.find(session);
	if (it == m_remoteStreams.end()) {
		return;
	}

	delete it->decoder;
	if (it->viewer) {
		it->viewer->disconnect(this);
		it->viewer->close();
	}
	m_remoteStreams.erase(it);
}

void ScreenShareManager::receiveFrame(ClientUser *user, const QByteArray &frameData, bool isKeyframe,
									  uint64_t frameNumber) {
	Q_UNUSED(frameNumber);

	if (!user) {
		return;
	}

	auto it = m_remoteStreams.find(user->uiSession);
	if (it == m_remoteStreams.end()) {
		return;
	}

	if (it->decoder) {
		it->decoder->decodeFrame(frameData, isKeyframe);
	}
}

void ScreenShareManager::onViewerClosed() {
	auto *viewer = qobject_cast< ScreenShareViewer * >(sender());
	if (!viewer) {
		return;
	}

	for (auto it = m_remoteStreams.begin(); it != m_remoteStreams.end(); ++it) {
		if (it->viewer == viewer) {
			delete it->decoder;
			it->viewer = nullptr; // Already closing via WA_DeleteOnClose
			m_remoteStreams.erase(it);
			return;
		}
	}
}
