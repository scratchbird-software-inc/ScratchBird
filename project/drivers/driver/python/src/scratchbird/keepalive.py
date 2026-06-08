# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""
Connection Keepalive Module
Prevents connection timeouts by periodically validating idle connections
"""

import asyncio
import time
from dataclasses import dataclass
from typing import Callable, Dict, Optional, Set
from datetime import datetime, timedelta
import threading


@dataclass
class KeepaliveConfig:
    """Configuration for connection keepalive"""
    interval: float = 120.0  # How often to check idle connections (seconds)
    max_idle_before_check: float = 600.0  # Max idle time before validation (seconds)
    validation_timeout: float = 5.0  # Timeout for validation query (seconds)


class KeepaliveTracker:
    """Tracks activity time for a single connection"""
    
    def __init__(self, config: KeepaliveConfig):
        self.config = config
        self._last_activity = time.monotonic()
        self._lock = threading.Lock()
    
    def mark_active(self):
        """Mark the connection as active"""
        with self._lock:
            self._last_activity = time.monotonic()
    
    def needs_validation(self) -> bool:
        """Check if connection needs validation"""
        with self._lock:
            idle_time = time.monotonic() - self._last_activity
            return idle_time > self.config.max_idle_before_check
    
    def idle_duration(self) -> float:
        """Get idle duration in seconds"""
        with self._lock:
            return time.monotonic() - self._last_activity


class KeepaliveManager:
    """Manages keepalive for multiple connections"""
    
    def __init__(self, config: KeepaliveConfig = None):
        self.config = config or KeepaliveConfig()
        self._trackers: Dict[str, KeepaliveTracker] = {}
        self._pingers: Dict[str, Callable[[], bool]] = {}
        self._lock = threading.Lock()
        self._stop_event = threading.Event()
        self._thread: Optional[threading.Thread] = None
    
    def start(self):
        """Start the keepalive monitoring thread"""
        self._thread = threading.Thread(target=self._monitor_loop, daemon=True)
        self._thread.start()
    
    def stop(self):
        """Stop the keepalive monitoring thread"""
        self._stop_event.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=5.0)
    
    def register(self, conn_id: str, pinger: Callable[[], bool]) -> KeepaliveTracker:
        """
        Register a connection for keepalive monitoring
        
        Args:
            conn_id: Unique connection identifier
            pinger: Function that returns True if connection is healthy
        
        Returns:
            KeepaliveTracker for the connection
        """
        tracker = KeepaliveTracker(self.config)
        
        with self._lock:
            self._trackers[conn_id] = tracker
            self._pingers[conn_id] = pinger
        
        return tracker
    
    def unregister(self, conn_id: str):
        """Unregister a connection from keepalive monitoring"""
        with self._lock:
            self._trackers.pop(conn_id, None)
            self._pingers.pop(conn_id, None)
    
    def _monitor_loop(self):
        """Background thread that periodically checks connections"""
        while not self._stop_event.is_set():
            self._check_connections()
            self._stop_event.wait(timeout=self.config.interval)
    
    def _check_connections(self):
        """Validate idle connections"""
        with self._lock:
            trackers = dict(self._trackers)
            pingers = dict(self._pingers)
        
        for conn_id, tracker in trackers.items():
            if tracker.needs_validation():
                pinger = pingers.get(conn_id)
                if pinger:
                    try:
                        # Run ping with timeout
                        healthy = self._ping_with_timeout(
                            pinger, 
                            self.config.validation_timeout
                        )
                        if healthy:
                            tracker.mark_active()
                        # If not healthy, connection will be replaced on next checkout
                    except Exception:
                        # Ping failed - connection likely dead
                        pass
    
    def _ping_with_timeout(self, pinger: Callable[[], bool], timeout: float) -> bool:
        """Run a ping function with timeout"""
        import concurrent.futures
        
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(pinger)
            try:
                return future.result(timeout=timeout)
            except concurrent.futures.TimeoutError:
                return False


class AsyncKeepaliveManager:
    """Async version of KeepaliveManager"""
    
    def __init__(self, config: KeepaliveConfig = None):
        self.config = config or KeepaliveConfig()
        self._trackers: Dict[str, KeepaliveTracker] = {}
        self._pingers: Dict[str, Callable[[], asyncio.Future]] = {}
        self._lock = asyncio.Lock()
        self._task: Optional[asyncio.Task] = None
        self._stop_event = asyncio.Event()
    
    async def start(self):
        """Start the keepalive monitoring task"""
        self._task = asyncio.create_task(self._monitor_loop())
    
    async def stop(self):
        """Stop the keepalive monitoring task"""
        self._stop_event.set()
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
    
    async def register(
        self, 
        conn_id: str, 
        pinger: Callable[[], asyncio.Future]
    ) -> KeepaliveTracker:
        """Register a connection for keepalive monitoring"""
        tracker = KeepaliveTracker(self.config)
        
        async with self._lock:
            self._trackers[conn_id] = tracker
            self._pingers[conn_id] = pinger
        
        return tracker
    
    async def unregister(self, conn_id: str):
        """Unregister a connection"""
        async with self._lock:
            self._trackers.pop(conn_id, None)
            self._pingers.pop(conn_id, None)
    
    async def _monitor_loop(self):
        """Background task that periodically checks connections"""
        while not self._stop_event.is_set():
            try:
                await asyncio.wait_for(
                    self._stop_event.wait(), 
                    timeout=self.config.interval
                )
            except asyncio.TimeoutError:
                await self._check_connections()
    
    async def _check_connections(self):
        """Validate idle connections"""
        async with self._lock:
            trackers = dict(self._trackers)
            pingers = dict(self._pingers)
        
        for conn_id, tracker in trackers.items():
            if tracker.needs_validation():
                pinger = pingers.get(conn_id)
                if pinger:
                    try:
                        # Run ping with timeout
                        healthy = await asyncio.wait_for(
                            pinger(),
                            timeout=self.config.validation_timeout
                        )
                        if healthy:
                            tracker.mark_active()
                    except asyncio.TimeoutError:
                        # Ping timed out - connection likely dead
                        pass
                    except Exception:
                        # Ping failed
                        pass
