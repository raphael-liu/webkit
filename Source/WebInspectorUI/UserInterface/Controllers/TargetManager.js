/*
 * Copyright (C) 2016-2018 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

WI.TargetManager = class TargetManager extends WI.Object
{
    constructor()
    {
        super();

        this._targets = new Map;
        this._cachedTargetsList = null;
        this._seenPageTarget = false;
        this._transitionTimeoutIdentifier = undefined;

        this._provisionalTargetInfos = new Map;
        this._swappedTargetIds = new Set;
    }

    // Public

    get targets()
    {
        if (!this._cachedTargetsList)
            this._cachedTargetsList = Array.from(this._targets.values()).filter((target) => !(target instanceof WI.MultiplexingBackendTarget));
        return this._cachedTargetsList;
    }

    get allTargets()
    {
        return Array.from(this._targets.values());
    }

    targetForIdentifier(targetId)
    {
        if (!targetId)
            return null;

        for (let target of this._targets.values()) {
            if (target.identifier === targetId)
                return target;
        }

        return null;
    }

    addTarget(target)
    {
        console.assert(target);
        console.assert(!this._targets.has(target.identifier));

        this._cachedTargetsList = null;
        this._targets.set(target.identifier, target);

        this.dispatchEventToListeners(WI.TargetManager.Event.TargetAdded, {target});
    }

    removeTarget(target)
    {
        console.assert(target);
        console.assert(target !== WI.mainTarget);

        this._cachedTargetsList = null;
        this._targets.delete(target.identifier);
        target.destroy();

        this.dispatchEventToListeners(WI.TargetManager.Event.TargetRemoved, {target});
    }

    createMultiplexingBackendTarget()
    {
        console.assert(WI.sharedApp.debuggableType === WI.DebuggableType.WebPage);

        let target = new WI.MultiplexingBackendTarget;
        target.initialize();

        this._initializeBackendTarget(target);

        // Add the target without dispatching an event.
        this._targets.set(target.identifier, target);
    }

    createDirectBackendTarget()
    {
        console.assert(WI.sharedApp.debuggableType !== WI.DebuggableType.WebPage);

        let target = new WI.DirectBackendTarget;
        target.initialize();

        this._initializeBackendTarget(target);

        if (WI.sharedApp.debuggableType === WI.DebuggableType.Page)
            this._initializePageTarget(target);

        this.addTarget(target);
    }

    // TargetObserver

    targetCreated(parentTarget, targetInfo)
    {
        this._connectToTarget(parentTarget, targetInfo);

        this.dispatchEventToListeners(WI.TargetManager.Event.TargetCreated, {targetInfo});
    }

    didCommitProvisionalTarget(parentTarget, previousTargetId, newTargetId)
    {
        this._destroyTarget(previousTargetId);
        let targetInfo = this._provisionalTargetInfos.get(newTargetId);
        console.assert(targetInfo);
        targetInfo.isProvisional = false;
        this._provisionalTargetInfos.delete(newTargetId);
        this._connectToTarget(parentTarget, targetInfo);
        console.assert(!this._swappedTargetIds.has(previousTargetId));
        this._swappedTargetIds.add(previousTargetId);

        this.dispatchEventToListeners(WI.TargetManager.Event.DidCommitProvisionalTarget, {previousTargetId, targetInfo});
    }

    targetDestroyed(targetId)
    {
        this._destroyTarget(targetId);

        this.dispatchEventToListeners(WI.TargetManager.Event.TargetDestroyed, {targetId});
    }

    dispatchMessageFromTarget(targetId, message)
    {
        if (this._provisionalTargetInfos.has(targetId))
            return;

        let target = this._targets.get(targetId);
        console.assert(target);
        if (!target)
            return;

        target.connection.dispatch(message);
    }

    // Private

    _connectToTarget(parentTarget, targetInfo)
    {
        console.assert(!this._provisionalTargetInfos.has(targetInfo.targetId));

        // COMPATIBILITY (iOS 13.0): `Target.TargetInfo.isProvisional` did not exist yet.
        if (targetInfo.isProvisional) {
            this._provisionalTargetInfos.set(targetInfo.targetId, targetInfo);
            return;
        }

        console.assert(parentTarget.hasCommand("Target.sendMessageToTarget"));
        let connection = new InspectorBackend.TargetConnection;
        let subTarget = this._createTarget(parentTarget, targetInfo, connection);
        this._checkAndHandlePageTargetTransition(subTarget);
        subTarget.initialize();

        this.addTarget(subTarget);
    }

    _destroyTarget(targetId)
    {
        if (this._provisionalTargetInfos.delete(targetId))
            return;

        if (this._swappedTargetIds.delete(targetId))
            return;

        let target = this._targets.get(targetId);
        this._checkAndHandlePageTargetTermination(target);
        this.removeTarget(target);
    }

    _createTarget(parentTarget, targetInfo, connection)
    {
        let {targetId, type} = targetInfo;

        switch (type) {
        case InspectorBackend.Enum.Target.TargetInfoType.Page:
            return new WI.PageTarget(parentTarget, targetId, WI.UIString("Page"), connection);
        case InspectorBackend.Enum.Target.TargetInfoType.Worker:
            return new WI.WorkerTarget(parentTarget, targetId, WI.UIString("Worker"), connection);
        case "serviceworker": // COMPATIBILITY (iOS 13): "serviceworker" was renamed to "service-worker".
        case InspectorBackend.Enum.Target.TargetInfoType.ServiceWorker:
            return new WI.WorkerTarget(parentTarget, targetId, WI.UIString("ServiceWorker"), connection);
        }

        throw "Unknown Target type: " + type;
    }

    _checkAndHandlePageTargetTransition(target)
    {
        if (target.type !== WI.TargetType.Page)
            return;

        // First page target.
        if (!WI.pageTarget && !this._seenPageTarget) {
            this._seenPageTarget = true;
            this._initializePageTarget(target);
            return;
        }

        // Transitioning page target.
        this._transitionPageTarget(target);
    }

    _checkAndHandlePageTargetTermination(target)
    {
        if (target.type !== WI.TargetType.Page)
            return;

        console.assert(target === WI.pageTarget);
        console.assert(this._seenPageTarget);

        // Terminating the page target.
        this._terminatePageTarget(target);

        // Ensure we transition in a reasonable amount of time, otherwise close.
        const timeToTransition = 2000;
        clearTimeout(this._transitionTimeoutIdentifier);
        this._transitionTimeoutIdentifier = setTimeout(() => {
            this._transitionTimeoutIdentifier = undefined;
            if (WI.pageTarget)
                return;
            if (WI.isEngineeringBuild)
                throw new Error("Error: No new pageTarget some time after last page target was terminated. Failed transition?");
            WI.close();
        }, timeToTransition);
    }

    _initializeBackendTarget(target)
    {
        console.assert(!WI.mainTarget);

        WI.backendTarget = target;

        this._resetMainExecutionContext();

        WI._backendTargetAvailablePromise.resolve();
    }

    _initializePageTarget(target)
    {
        console.assert(WI.sharedApp.isWebDebuggable());
        console.assert(target.type === WI.TargetType.Page || target instanceof WI.DirectBackendTarget);

        WI.pageTarget = target;

        this._resetMainExecutionContext();

        WI._pageTargetAvailablePromise.resolve();
    }

    _transitionPageTarget(target)
    {
        console.assert(!WI.pageTarget);
        console.assert(WI.sharedApp.debuggableType === WI.DebuggableType.WebPage);
        console.assert(target.type === WI.TargetType.Page);

        WI.pageTarget = target;

        this._resetMainExecutionContext();

        // Actions to transition the page target.
        WI.notifications.dispatchEventToListeners(WI.Notification.TransitionPageTarget);
        WI.domManager.transitionPageTarget();
        WI.networkManager.transitionPageTarget();
        WI.timelineManager.transitionPageTarget();
    }

    _terminatePageTarget(target)
    {
        console.assert(WI.pageTarget);
        console.assert(WI.pageTarget === target);
        console.assert(WI.sharedApp.debuggableType === WI.DebuggableType.WebPage);

        // Remove any Worker targets associated with this page.
        let workerTargets = WI.targets.filter((x) => x.type === WI.TargetType.Worker);
        for (let workerTarget of workerTargets)
            WI.workerManager.workerTerminated(workerTarget.identifier);

        WI.pageTarget = null;
    }

    _resetMainExecutionContext()
    {
        if (WI.mainTarget instanceof WI.MultiplexingBackendTarget)
            return;

        if (WI.mainTarget.executionContext)
            WI.runtimeManager.activeExecutionContext = WI.mainTarget.executionContext;
    }
};

WI.TargetManager.Event = {
    TargetAdded: Symbol("target-manager-target-added"),
    TargetRemoved: Symbol("target-manager-target-removed"),

    TargetCreated: "target-manager-target-created",
    DidCommitProvisionalTarget: "target-manager-provisional-target-committed",
    TargetDestroyed: "target-manager-target-detroyed",
};
