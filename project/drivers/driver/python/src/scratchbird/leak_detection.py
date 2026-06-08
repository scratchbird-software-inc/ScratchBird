# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""
Connection Leak Detection Module
Detects when connections are held longer than expected
"""

import threading
import time
import traceback
import uuid
from dataclasses import dataclass, field
from datetime import datetime
from typing import Dict, Optional, Callable, List
from enum import Enum
import logging

logger = logging.getLogger(__name__)


class LeakLogLevel(Enum):
    DEBUG = "debug"
    WARNING = "warning"
    ERROR = "error"


@dataclass
class LeakDetectionConfig:
    """Configuration for leak detection"""
    threshold: float = 30.0  # Seconds before warning about leak
    capture_stack_trace: bool = False
    check_interval: float = 10.0  # How often to check for leaks
    log_level: LeakLogLevel = LeakLogLevel.WARNING


@dataclass
class CheckoutInfo:
    """Information about a connection checkout"""
    checkout_time: float
    thread_id: int
    stack_trace: Optional[str]
    metadata: Dict[str, str] = field(default_factory=dict)
    
    def held_duration(self) -> float:
        """How long the connection has been held (seconds)"""
        return time.monotonic() - self.checkout_time


class LeakDetector:
    """Detects potential connection leaks"""
    
    def __init__(self, config: LeakDetectionConfig = None):
        self.config = config or LeakDetectionConfig()
        self._checkouts: Dict[str, CheckoutInfo] = {}
        self._lock = threading.Lock()
        self._stop_event = threading.Event()
        self._monitor_thread: Optional[threading.Thread] = None
    
    def start(self):
        """Start the leak detection monitoring thread"""
        self._monitor_thread = threading.Thread(
            target=self._monitor_loop, 
            daemon=True
        )
        self._monitor_thread.start()
    
    def stop(self):
        """Stop the leak detection monitoring thread"""
        self._stop_event.set()
        if self._monitor_thread and self._monitor_thread.is_alive():
            self._monitor_thread.join(timeout=5.0)
    
    def checkout(
        self, 
        conn_id: str, 
        metadata: Dict[str, str] = None
    ) -> 'LeakDetectionGuard':
        """
        Register a connection checkout
        
        Returns a guard that should be released when connection is returned
        """
        stack_trace = None
        if self.config.capture_stack_trace:
            stack_trace = ''.join(traceback.format_stack()[:-1])
        
        info = CheckoutInfo(
            checkout_time=time.monotonic(),
            thread_id=threading.get_ident(),
            stack_trace=stack_trace,
            metadata=metadata or {}
        )
        
        with self._lock:
            self._checkouts[conn_id] = info
        
        logger.debug(f"Connection {conn_id} checked out")
        
        return LeakDetectionGuard(conn_id, self)
    
    def checkin(self, conn_id: str):
        """Register a connection return"""
        with self._lock:
            info = self._checkouts.pop(conn_id, None)
        
        if info:
            held_duration = info.held_duration()
            
            if held_duration > self.config.threshold:
                # Connection was held longer than threshold but returned
                logger.warning(
                    f"Connection {conn_id} held for {held_duration:.2f}s "
                    f"(threshold: {self.config.threshold}s) - returned"
                )
            else:
                logger.debug(
                    f"Connection {conn_id} returned after {held_duration:.2f}s"
                )
    
    def active_checkouts(self) -> Dict[str, CheckoutInfo]:
        """Get information about all active checkouts"""
        with self._lock:
            return dict(self._checkouts)
    
    def active_count(self) -> int:
        """Get number of active checkouts"""
        with self._lock:
            return len(self._checkouts)
    
    def stats(self) -> Dict[str, int]:
        """Get leak detection statistics"""
        with self._lock:
            checkouts = dict(self._checkouts)
        
        potential_leaks = sum(
            1 for info in checkouts.values()
            if info.held_duration() > self.config.threshold
        )
        
        return {
            'active_checkouts': len(checkouts),
            'potential_leaks': potential_leaks
        }
    
    def _monitor_loop(self):
        """Background thread that checks for leaks"""
        while not self._stop_event.is_set():
            self._check_leaks()
            self._stop_event.wait(timeout=self.config.check_interval)
    
    def _check_leaks(self):
        """Check for connections held beyond threshold"""
        with self._lock:
            checkouts = list(self._checkouts.items())
        
        for conn_id, info in checkouts:
            held_duration = info.held_duration()
            
            if held_duration > self.config.threshold:
                self._log_leak(conn_id, info, held_duration)
    
    def _log_leak(self, conn_id: str, info: CheckoutInfo, duration: float):
        """Log a potential connection leak"""
        message = (
            f"POSSIBLE CONNECTION LEAK: conn_id={conn_id}, "
            f"held={duration:.2f}s, threshold={self.config.threshold}s, "
            f"thread={info.thread_id}, metadata={info.metadata}"
        )
        
        if self.config.log_level == LeakLogLevel.DEBUG:
            logger.debug(message)
        elif self.config.log_level == LeakLogLevel.WARNING:
            logger.warning(message)
        else:
            logger.error(message)
        
        if info.stack_trace:
            logger.warning(f"Stack trace for connection {conn_id}:\n{info.stack_trace}")


class LeakDetectionGuard:
    """RAII guard for leak detection - automatically calls checkin on release"""
    
    def __init__(self, conn_id: str, detector: LeakDetector):
        self.conn_id = conn_id
        self.detector = detector
        self._released = False
    
    def release(self):
        """Release the connection (mark as returned)"""
        if not self._released:
            self.detector.checkin(self.conn_id)
            self._released = True
    
    def __del__(self):
        """Destructor - ensures checkin is called"""
        self.release()
    
    def __enter__(self):
        """Context manager entry"""
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb):
        """Context manager exit - always releases"""
        self.release()


class AsyncLeakDetector:
    """Async version of LeakDetector"""
    
    def __init__(self, config: LeakDetectionConfig = None):
        self.config = config or LeakDetectionConfig()
        self._checkouts: Dict[str, CheckoutInfo] = {}
        self._lock = asyncio.Lock()
        self._task: Optional[asyncio.Task] = None
        self._stop_event = asyncio.Event()
    
    async def start(self):
        """Start the leak detection monitoring task"""
        self._task = asyncio.create_task(self._monitor_loop())
    
    async def stop(self):
        """Stop the leak detection monitoring task"""
        self._stop_event.set()
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
    
    async def checkout(
        self, 
        conn_id: str, 
        metadata: Dict[str, str] = None
    ) -> 'AsyncLeakDetectionGuard':
        """Register a connection checkout"""
        import asyncio
        
        stack_trace = None
        if self.config.capture_stack_trace:
            stack_trace = ''.join(traceback.format_stack()[:-1])
        
        info = CheckoutInfo(
            checkout_time=time.monotonic(),
            thread_id=threading.get_ident(),
            stack_trace=stack_trace,
            metadata=metadata or {}
        )
        
        async with self._lock:
            self._checkouts[conn_id] = info
        
        return AsyncLeakDetectionGuard(conn_id, self)
    
    async def checkin(self, conn_id: str):
        """Register a connection return"""
        async with self._lock:
            info = self._checkouts.pop(conn_id, None)
        
        if info:
            held_duration = info.held_duration()
            
            if held_duration > self.config.threshold:
                logger.warning(
                    f"Connection {conn_id} held for {held_duration:.2f}s "
                    f"(threshold: {self.config.threshold}s) - returned"
                )
    
    async def _monitor_loop(self):
        """Background task that checks for leaks"""
        import asyncio
        
        while not self._stop_event.is_set():
            try:
                await asyncio.wait_for(
                    self._stop_event.wait(),
                    timeout=self.config.check_interval
                )
            except asyncio.TimeoutError:
                await self._check_leaks()
    
    async def _check_leaks(self):
        """Check for connections held beyond threshold"""
        async with self._lock:
            checkouts = list(self._checkouts.items())
        
        for conn_id, info in checkouts:
            if info.held_duration() > self.config.threshold:
                self._log_leak(conn_id, info, info.held_duration())
    
    def _log_leak(self, conn_id: str, info: CheckoutInfo, duration: float):
        """Log a potential connection leak"""
        message = (
            f"POSSIBLE CONNECTION LEAK: conn_id={conn_id}, "
            f"held={duration:.2f}s"
        )
        
        if self.config.log_level == LeakLogLevel.WARNING:
            logger.warning(message)
        elif self.config.log_level == LeakLogLevel.ERROR:
            logger.error(message)
        else:
            logger.debug(message)


class AsyncLeakDetectionGuard:
    """Async version of LeakDetectionGuard"""
    
    def __init__(self, conn_id: str, detector: AsyncLeakDetector):
        self.conn_id = conn_id
        self.detector = detector
        self._released = False
    
    async def release(self):
        """Release the connection"""
        if not self._released:
            await self.detector.checkin(self.conn_id)
            self._released = True
    
    async def __aenter__(self):
        """Async context manager entry"""
        return self
    
    async def __aexit__(self, exc_type, exc_val, exc_tb):
        """Async context manager exit"""
        await self.release()
