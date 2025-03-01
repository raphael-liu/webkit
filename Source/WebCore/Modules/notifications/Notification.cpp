/*
 * Copyright (C) 2009 Google Inc. All rights reserved.
 * Copyright (C) 2009, 2011, 2012, 2016 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#if ENABLE(NOTIFICATIONS)

#include "Notification.h"

#include "Document.h"
#include "Event.h"
#include "EventNames.h"
#include "NotificationClient.h"
#include "NotificationController.h"
#include "NotificationPermissionCallback.h"
#include "WindowEventLoop.h"
#include "WindowFocusAllowedIndicator.h"
#include <wtf/IsoMallocInlines.h>

namespace WebCore {

WTF_MAKE_ISO_ALLOCATED_IMPL(Notification);

Ref<Notification> Notification::create(Document& context, const String& title, const Options& options)
{
    auto notification = adoptRef(*new Notification(context, title, options));
    notification->suspendIfNeeded();
    return notification;
}

Notification::Notification(Document& document, const String& title, const Options& options)
    : ActiveDOMObject(document)
    , m_title(title)
    , m_direction(options.dir)
    , m_lang(options.lang)
    , m_body(options.body)
    , m_tag(options.tag)
    , m_state(Idle)
    , m_showNotificationTimer(&document, *this, &Notification::show)
{
    if (!options.icon.isEmpty()) {
        auto iconURL = document.completeURL(options.icon);
        if (iconURL.isValid())
            m_icon = iconURL;
    }

    m_showNotificationTimer.startOneShot(0_s);
    m_showNotificationTimer.suspendIfNeeded();
}

Notification::~Notification() = default;

void Notification::show()
{
    // prevent double-showing
    if (m_state != Idle)
        return;

    auto* page = document()->page();
    if (!page)
        return;

    auto& client = NotificationController::from(page)->client();

    if (client.checkPermission(scriptExecutionContext()) != Permission::Granted) {
        dispatchErrorEvent();
        return;
    }
    if (client.show(this)) {
        m_state = Showing;
        setPendingActivity(*this);
    }
}

void Notification::close()
{
    switch (m_state) {
    case Idle:
        break;
    case Showing: {
        if (auto* page = document()->page())
            NotificationController::from(page)->client().cancel(this);
        break;
    }
    case Closed:
        break;
    }
}

Document* Notification::document() const
{
    return downcast<Document>(scriptExecutionContext());
}

const char* Notification::activeDOMObjectName() const
{
    return "Notification";
}

void Notification::stop()
{
    ActiveDOMObject::stop();

    if (auto* page = document()->page())
        NotificationController::from(page)->client().notificationObjectDestroyed(this);
}

void Notification::suspend(ReasonForSuspension)
{
    close();
}

void Notification::finalize()
{
    if (m_state == Closed)
        return;
    m_state = Closed;
    unsetPendingActivity(*this);
}

void Notification::queueTask(Function<void()>&& task)
{
    auto* document = this->document();
    if (!document)
        return;

    document->eventLoop().queueTask(TaskSource::UserInteraction, WTFMove(task));
}

void Notification::dispatchShowEvent()
{
    queueTask([this, pendingActivity = makePendingActivity(*this)] {
        dispatchEvent(Event::create(eventNames().showEvent, Event::CanBubble::No, Event::IsCancelable::No));
    });
}

void Notification::dispatchClickEvent()
{
    queueTask([this, pendingActivity = makePendingActivity(*this)] {
        WindowFocusAllowedIndicator windowFocusAllowed;
        dispatchEvent(Event::create(eventNames().clickEvent, Event::CanBubble::No, Event::IsCancelable::No));
    });
}

void Notification::dispatchCloseEvent()
{
    queueTask([this, pendingActivity = makePendingActivity(*this)] {
        dispatchEvent(Event::create(eventNames().closeEvent, Event::CanBubble::No, Event::IsCancelable::No));
    });
    finalize();
}

void Notification::dispatchErrorEvent()
{
    queueTask([this, pendingActivity = makePendingActivity(*this)] {
        dispatchEvent(Event::create(eventNames().errorEvent, Event::CanBubble::No, Event::IsCancelable::No));
    });
}

auto Notification::permission(Document& document) -> Permission
{
    auto* page = document.page();
    if (!page)
        return Permission::Default;

    return NotificationController::from(document.page())->client().checkPermission(&document);
}

void Notification::requestPermission(Document& document, RefPtr<NotificationPermissionCallback>&& callback)
{
    auto* page = document.page();
    if (!page)
        return;

    NotificationController::from(page)->client().requestPermission(&document, WTFMove(callback));
}

} // namespace WebCore

#endif // ENABLE(NOTIFICATIONS)
