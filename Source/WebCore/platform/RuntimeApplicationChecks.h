/*
 * Copyright (C) 2009-2017 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer. 
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution. 
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/Forward.h>

namespace WebCore {

WEBCORE_EXPORT void setPresentingApplicationPID(int);
WEBCORE_EXPORT int presentingApplicationPID();

#if PLATFORM(WIN)
inline bool isInWebProcess() { return false; }
#elif !PLATFORM(COCOA)
inline bool isInWebProcess() { return true; }
#endif

#if PLATFORM(COCOA)

bool isInWebProcess();
bool isInNetworkProcess();

WEBCORE_EXPORT void setApplicationSDKVersion(uint32_t);
WEBCORE_EXPORT uint32_t applicationSDKVersion();

WEBCORE_EXPORT void setApplicationBundleIdentifier(const String&);
String applicationBundleIdentifier();

#if PLATFORM(MAC)

namespace MacApplication {

WEBCORE_EXPORT bool isAOLInstantMessenger();
WEBCORE_EXPORT bool isAdobeInstaller();
WEBCORE_EXPORT bool isAperture();
WEBCORE_EXPORT bool isAppleMail();
WEBCORE_EXPORT bool isIBooks();
WEBCORE_EXPORT bool isITunes();
WEBCORE_EXPORT bool isMicrosoftMessenger();
WEBCORE_EXPORT bool isMicrosoftMyDay();
WEBCORE_EXPORT bool isMicrosoftOutlook();
bool isQuickenEssentials();
WEBCORE_EXPORT bool isSafari();
bool isSolidStateNetworksDownloader();
WEBCORE_EXPORT bool isVersions();
WEBCORE_EXPORT bool isHRBlock();
WEBCORE_EXPORT bool isIAdProducer();
WEBCORE_EXPORT bool isEpsonSoftwareUpdater();

} // MacApplication

#endif // PLATFORM(MAC)

#if PLATFORM(IOS_FAMILY)

namespace IOSApplication {

WEBCORE_EXPORT bool isMobileMail();
WEBCORE_EXPORT bool isMailCompositionService();
WEBCORE_EXPORT bool isMobileSafari();
WEBCORE_EXPORT bool isSafariViewService();
WEBCORE_EXPORT bool isIMDb();
WEBCORE_EXPORT bool isWebBookmarksD();
WEBCORE_EXPORT bool isDumpRenderTree();
bool isMobileStore();
bool isSpringBoard();
WEBCORE_EXPORT bool isWebProcess();
WEBCORE_EXPORT bool isIBooks();
bool isIBooksStorytime();
WEBCORE_EXPORT bool isTheSecretSocietyHiddenMystery();
WEBCORE_EXPORT bool isCardiogram();
WEBCORE_EXPORT bool isNike();
bool isMoviStarPlus();
WEBCORE_EXPORT bool isFirefox();
WEBCORE_EXPORT bool isAppleApplication();
WEBCORE_EXPORT bool isEvernote();
WEBCORE_EXPORT bool isEventbrite();
WEBCORE_EXPORT bool isDataActivation();

} // IOSApplication

#endif // PLATFORM(IOS_FAMILY)

#else

inline bool isInNetworkProcess() { return false; }

#endif // PLATFORM(COCOA)

} // namespace WebCore
