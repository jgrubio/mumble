// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ScreenShareViewer.h"

#include <QCloseEvent>
#include <QVBoxLayout>

ScreenShareViewer::ScreenShareViewer(const QString &userName, QWidget *parent) : QWidget(parent) {
	setWindowTitle(tr("Screen Share - %1").arg(userName));
	setAttribute(Qt::WA_DeleteOnClose);
	resize(1920, 1080);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	m_label = new QLabel(this);
	m_label->setAlignment(Qt::AlignCenter);
	m_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	m_label->setMinimumSize(320, 180);
	layout->addWidget(m_label);
}

ScreenShareViewer::~ScreenShareViewer() = default;

void ScreenShareViewer::updateFrame(QImage frame) {
	m_currentFrame = frame;
	updatePixmap();
}

void ScreenShareViewer::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);
	updatePixmap();
}

void ScreenShareViewer::closeEvent(QCloseEvent *event) {
	emit viewerClosed();
	QWidget::closeEvent(event);
}

void ScreenShareViewer::updatePixmap() {
	if (m_currentFrame.isNull()) {
		return;
	}

	QPixmap pix = QPixmap::fromImage(m_currentFrame);
	m_label->setPixmap(pix.scaled(m_label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
