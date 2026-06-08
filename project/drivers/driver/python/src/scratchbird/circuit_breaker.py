# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""
Circuit Breaker Module
Prevents cascading failures by stopping requests after consecutive failures
"""

import asyncio
import threading
import time
from dataclasses import dataclass
from enum import Enum, auto
from typing import Callable, Optional, TypeVar, Generic
import logging

logger = logging.getLogger(__name__)


class CircuitState(Enum):
    """States of the circuit breaker"""
    CLOSED = auto()      # Normal operation
    OPEN = auto()        # Failure threshold reached, requests blocked
    HALF_OPEN = auto()   # Testing if service recovered


@dataclass
class CircuitBreakerConfig:
    """Configuration for circuit breaker"""
    failure_threshold: int = 5
    recovery_timeout: float = 30.0  # Seconds
    success_threshold: int = 3
    half_open_max_requests: int = 10


class CircuitBreakerError(Exception):
    """Raised when circuit breaker is open"""
    pass


T = TypeVar('T')


class CircuitBreaker:
    """Circuit breaker implementation"""
    
    def __init__(
        self, 
        config: CircuitBreakerConfig = None,
        name: str = "default"
    ):
        self.config = config or CircuitBreakerConfig()
        self.name = name
        self._state = CircuitState.CLOSED
        self._failure_count = 0
        self._success_count = 0
        self._half_open_requests = 0
        self._last_failure_time: Optional[float] = None
        self._lock = threading.Lock()
    
    @property
    def state(self) -> CircuitState:
        """Current circuit state"""
        with self._lock:
            return self._state
    
    def allow_request(self) -> bool:
        """Check if a request should be allowed"""
        with self._lock:
            return self._allow_request_locked()
    
    def _allow_request_locked(self) -> bool:
        """Internal method with lock held"""
        if self._state == CircuitState.CLOSED:
            return True
        
        elif self._state == CircuitState.OPEN:
            # Check if recovery timeout has passed
            if (self._last_failure_time and 
                time.monotonic() - self._last_failure_time >= self.config.recovery_timeout):
                # Transition to half-open
                logger.info(f"Circuit {self.name}: OPEN -> HALF_OPEN")
                self._state = CircuitState.HALF_OPEN
                self._failure_count = 0
                self._success_count = 0
                self._half_open_requests = 0
                return self._allow_half_open_request()
            
            return False
        
        elif self._state == CircuitState.HALF_OPEN:
            return self._allow_half_open_request()
        
        return False
    
    def _allow_half_open_request(self) -> bool:
        """Allow request in half-open state (limited)"""
        if self._half_open_requests < self.config.half_open_max_requests:
            self._half_open_requests += 1
            return True
        return False
    
    def record_success(self):
        """Record a successful operation"""
        with self._lock:
            if self._state == CircuitState.CLOSED:
                self._failure_count = 0
            
            elif self._state == CircuitState.HALF_OPEN:
                self._half_open_requests -= 1
                self._success_count += 1
                
                if self._success_count >= self.config.success_threshold:
                    logger.info(f"Circuit {self.name}: HALF_OPEN -> CLOSED")
                    self._state = CircuitState.CLOSED
                    self._failure_count = 0
                    self._success_count = 0
    
    def record_failure(self):
        """Record a failed operation"""
        with self._lock:
            if self._state == CircuitState.CLOSED:
                self._failure_count += 1
                
                if self._failure_count >= self.config.failure_threshold:
                    logger.warning(f"Circuit {self.name}: CLOSED -> OPEN")
                    self._state = CircuitState.OPEN
                    self._last_failure_time = time.monotonic()
            
            elif self._state == CircuitState.HALF_OPEN:
                self._half_open_requests -= 1
                logger.warning(f"Circuit {self.name}: HALF_OPEN -> OPEN")
                self._state = CircuitState.OPEN
                self._last_failure_time = time.monotonic()
            
            elif self._state == CircuitState.OPEN:
                self._last_failure_time = time.monotonic()
    
    def reset(self):
        """Manually reset to closed state"""
        with self._lock:
            old_state = self._state
            self._state = CircuitState.CLOSED
            self._failure_count = 0
            self._success_count = 0
            self._half_open_requests = 0
            if old_state != CircuitState.CLOSED:
                logger.info(f"Circuit {self.name}: manually reset to CLOSED")
    
    def call(self, func: Callable[[], T], *args, **kwargs) -> T:
        """
        Execute a function with circuit breaker protection
        
        Args:
            func: Function to execute
            *args: Positional arguments
            **kwargs: Keyword arguments
        
        Returns:
            Result of func(*args, **kwargs)
        
        Raises:
            CircuitBreakerError: If circuit is open
            Exception: If func raises an exception
        """
        if not self.allow_request():
            raise CircuitBreakerError(f"Circuit {self.name} is OPEN")
        
        try:
            result = func(*args, **kwargs)
            self.record_success()
            return result
        except Exception:
            self.record_failure()
            raise
    
    async def call_async(self, func: Callable[..., asyncio.Future], *args, **kwargs) -> T:
        """Async version of call"""
        if not self.allow_request():
            raise CircuitBreakerError(f"Circuit {self.name} is OPEN")
        
        try:
            result = await func(*args, **kwargs)
            self.record_success()
            return result
        except Exception:
            self.record_failure()
            raise
    
    @property
    def stats(self) -> dict:
        """Get circuit breaker statistics"""
        with self._lock:
            return {
                'state': self._state.name,
                'failure_count': self._failure_count,
                'success_count': self._success_count,
                'half_open_requests': self._half_open_requests,
                'last_failure_time': self._last_failure_time
            }
    
    def __repr__(self) -> str:
        stats = self.stats
        return (f"CircuitBreaker(name={self.name}, state={stats['state']}, "
                f"failures={stats['failure_count']})")


class AsyncCircuitBreaker:
    """Async version of CircuitBreaker"""
    
    def __init__(
        self, 
        config: CircuitBreakerConfig = None,
        name: str = "default"
    ):
        self.config = config or CircuitBreakerConfig()
        self.name = name
        self._state = CircuitState.CLOSED
        self._failure_count = 0
        self._success_count = 0
        self._half_open_requests = 0
        self._last_failure_time: Optional[float] = None
        self._lock = asyncio.Lock()
    
    @property
    async def state(self) -> CircuitState:
        """Current circuit state"""
        async with self._lock:
            return self._state
    
    async def allow_request(self) -> bool:
        """Check if a request should be allowed"""
        async with self._lock:
            return await self._allow_request_locked()
    
    async def _allow_request_locked(self) -> bool:
        """Internal method with lock held"""
        if self._state == CircuitState.CLOSED:
            return True
        
        elif self._state == CircuitState.OPEN:
            if (self._last_failure_time and 
                time.monotonic() - self._last_failure_time >= self.config.recovery_timeout):
                logger.info(f"Circuit {self.name}: OPEN -> HALF_OPEN")
                self._state = CircuitState.HALF_OPEN
                self._failure_count = 0
                self._success_count = 0
                self._half_open_requests = 0
                return await self._allow_half_open_request()
            
            return False
        
        elif self._state == CircuitState.HALF_OPEN:
            return await self._allow_half_open_request()
        
        return False
    
    async def _allow_half_open_request(self) -> bool:
        """Allow request in half-open state (limited)"""
        if self._half_open_requests < self.config.half_open_max_requests:
            self._half_open_requests += 1
            return True
        return False
    
    async def record_success(self):
        """Record a successful operation"""
        async with self._lock:
            if self._state == CircuitState.CLOSED:
                self._failure_count = 0
            
            elif self._state == CircuitState.HALF_OPEN:
                self._half_open_requests -= 1
                self._success_count += 1
                
                if self._success_count >= self.config.success_threshold:
                    logger.info(f"Circuit {self.name}: HALF_OPEN -> CLOSED")
                    self._state = CircuitState.CLOSED
                    self._failure_count = 0
                    self._success_count = 0
    
    async def record_failure(self):
        """Record a failed operation"""
        async with self._lock:
            if self._state == CircuitState.CLOSED:
                self._failure_count += 1
                
                if self._failure_count >= self.config.failure_threshold:
                    logger.warning(f"Circuit {self.name}: CLOSED -> OPEN")
                    self._state = CircuitState.OPEN
                    self._last_failure_time = time.monotonic()
            
            elif self._state == CircuitState.HALF_OPEN:
                self._half_open_requests -= 1
                logger.warning(f"Circuit {self.name}: HALF_OPEN -> OPEN")
                self._state = CircuitState.OPEN
                self._last_failure_time = time.monotonic()
            
            elif self._state == CircuitState.OPEN:
                self._last_failure_time = time.monotonic()
    
    async def reset(self):
        """Manually reset to closed state"""
        async with self._lock:
            old_state = self._state
            self._state = CircuitState.CLOSED
            self._failure_count = 0
            self._success_count = 0
            self._half_open_requests = 0
            if old_state != CircuitState.CLOSED:
                logger.info(f"Circuit {self.name}: manually reset to CLOSED")
    
    async def call(self, func: Callable[..., asyncio.Future], *args, **kwargs) -> T:
        """Execute an async function with circuit breaker protection"""
        if not await self.allow_request():
            raise CircuitBreakerError(f"Circuit {self.name} is OPEN")
        
        try:
            result = await func(*args, **kwargs)
            await self.record_success()
            return result
        except Exception:
            await self.record_failure()
            raise
    
    async def stats(self) -> dict:
        """Get circuit breaker statistics"""
        async with self._lock:
            return {
                'state': self._state.name,
                'failure_count': self._failure_count,
                'success_count': self._success_count,
                'half_open_requests': self._half_open_requests,
                'last_failure_time': self._last_failure_time
            }


def circuit_breaker(
    breaker: CircuitBreaker,
    fallback: Callable[[], T] = None
):
    """
    Decorator for circuit breaker protection
    
    Args:
        breaker: CircuitBreaker instance
        fallback: Optional fallback function if circuit is open
    """
    def decorator(func: Callable[..., T]) -> Callable[..., T]:
        def wrapper(*args, **kwargs) -> T:
            try:
                return breaker.call(lambda: func(*args, **kwargs))
            except CircuitBreakerError:
                if fallback:
                    return fallback()
                raise
        return wrapper
    return decorator


def async_circuit_breaker(
    breaker: AsyncCircuitBreaker,
    fallback: Callable[[], asyncio.Future] = None
):
    """Decorator for async circuit breaker protection"""
    def decorator(func: Callable[..., asyncio.Future]) -> Callable[..., asyncio.Future]:
        async def wrapper(*args, **kwargs) -> T:
            try:
                return await breaker.call(func, *args, **kwargs)
            except CircuitBreakerError:
                if fallback:
                    return await fallback()
                raise
        return wrapper
    return decorator
