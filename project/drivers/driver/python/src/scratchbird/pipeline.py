# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""
Query Pipelining Module
Batches multiple queries for efficiency
"""

import asyncio
import threading
from dataclasses import dataclass
from typing import List, Optional, Any, Callable, Dict
from concurrent.futures import Future
from queue import Queue, Empty


@dataclass
class PipelineConfig:
    """Configuration for query pipelining"""
    max_in_flight: int = 100
    auto_flush: bool = True
    auto_flush_threshold: int = 10
    flush_timeout: float = 5.0


class PipelinedRequest:
    """A pipelined query request"""
    
    def __init__(self, sql: str, params: Optional[List[Any]] = None):
        self.sql = sql
        self.params = params or []
        self.future: Future = Future()


class QueryPipeline:
    """Pipeline for batching queries"""
    
    def __init__(self, config: PipelineConfig = None):
        self.config = config or PipelineConfig()
        self._queue: Queue = Queue(maxsize=self.config.max_in_flight)
        self._in_flight = 0
        self._in_flight_lock = threading.Lock()
        self._running = False
        self._connection = None
        self._worker_thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()
    
    def start(self, connection) -> None:
        """Start the pipeline"""
        self._connection = connection
        self._running = True
        self._stop_event.clear()
        self._worker_thread = threading.Thread(target=self._process_loop)
        self._worker_thread.daemon = True
        self._worker_thread.start()
    
    def stop(self) -> None:
        """Stop the pipeline"""
        self._running = False
        self._stop_event.set()
        if self._worker_thread:
            self._worker_thread.join(timeout=5.0)
    
    def queue(self, sql: str, params: Optional[List[Any]] = None) -> Future:
        """Queue a query for execution"""
        if self._in_flight >= self.config.max_in_flight:
            future = Future()
            future.set_exception(Exception("Pipeline at capacity"))
            return future
        
        request = PipelinedRequest(sql, params)
        
        try:
            self._queue.put_nowait(request)
            
            # Auto-flush if threshold reached
            if self.config.auto_flush and self._queue.qsize() >= self.config.auto_flush_threshold:
                self.flush()
                
        except:
            request.future.set_exception(Exception("Failed to queue request"))
        
        return request.future
    
    def queue_sync(self, sql: str, params: Optional[List[Any]] = None) -> Any:
        """Queue a query and wait for result synchronously"""
        future = self.queue(sql, params)
        return future.result(timeout=self.config.flush_timeout)
    
    def pending_count(self) -> int:
        """Get number of pending requests"""
        return self._queue.qsize()
    
    def in_flight_count(self) -> int:
        """Get number of in-flight requests"""
        with self._in_flight_lock:
            return self._in_flight
    
    def has_capacity(self) -> bool:
        """Check if pipeline has capacity"""
        return self.in_flight_count() < self.config.max_in_flight
    
    def flush(self) -> None:
        """Trigger immediate processing of pending requests"""
        # Signal to process immediately - in threaded version, 
        # we just let the worker process what's in queue
        pass
    
    def _process_loop(self) -> None:
        """Main processing loop"""
        while self._running and not self._stop_event.is_set():
            batch = self._drain_batch()
            if batch:
                self._process_batch(batch)
            else:
                # Small sleep to prevent busy-waiting
                self._stop_event.wait(0.01)
    
    def _drain_batch(self) -> List[PipelinedRequest]:
        """Drain pending requests into a batch"""
        batch = []
        max_batch = self.config.auto_flush_threshold
        
        while len(batch) < max_batch:
            try:
                request = self._queue.get_nowait()
                batch.append(request)
            except Empty:
                break
        
        return batch
    
    def _process_batch(self, batch: List[PipelinedRequest]) -> None:
        """Process a batch of requests"""
        with self._in_flight_lock:
            self._in_flight += len(batch)
        
        try:
            for request in batch:
                try:
                    result = self._execute_request(request)
                    request.future.set_result(result)
                except Exception as e:
                    request.future.set_exception(e)
        finally:
            with self._in_flight_lock:
                self._in_flight -= len(batch)
    
    def _execute_request(self, request: PipelinedRequest) -> Any:
        """Execute a single request"""
        cursor = self._connection.cursor()
        try:
            cursor.execute(request.sql, request.params)
            
            # Determine if query or command
            if cursor.description:
                # SELECT query
                return cursor.fetchall()
            else:
                # INSERT/UPDATE/DELETE
                return cursor.rowcount
        finally:
            cursor.close()


class AsyncQueryPipeline:
    """Async version of QueryPipeline"""
    
    def __init__(self, config: PipelineConfig = None):
        self.config = config or PipelineConfig()
        self._queue: asyncio.Queue = asyncio.Queue(maxsize=self.config.max_in_flight)
        self._in_flight = 0
        self._running = False
        self._connection = None
        self._task: Optional[asyncio.Task] = None
    
    async def start(self, connection) -> None:
        """Start the pipeline"""
        self._connection = connection
        self._running = True
        self._task = asyncio.create_task(self._process_loop())
    
    async def stop(self) -> None:
        """Stop the pipeline"""
        self._running = False
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
    
    async def queue(self, sql: str, params: Optional[List[Any]] = None) -> asyncio.Future:
        """Queue a query for execution"""
        if self._in_flight >= self.config.max_in_flight:
            future = asyncio.Future()
            future.set_exception(Exception("Pipeline at capacity"))
            return future
        
        request = PipelinedRequest(sql, params)
        
        try:
            await asyncio.wait_for(
                self._queue.put(request),
                timeout=1.0
            )
            
            # Auto-flush
            if self.config.auto_flush and self._queue.qsize() >= self.config.auto_flush_threshold:
                await self.flush()
                
        except asyncio.TimeoutError:
            request.future.set_exception(Exception("Failed to queue request"))
        
        # Wrap threading.Future in asyncio.Future
        loop = asyncio.get_event_loop()
        async_future = loop.create_future()
        
        def on_done(f):
            try:
                result = f.result()
                loop.call_soon_threadsafe(async_future.set_result, result)
            except Exception as e:
                loop.call_soon_threadsafe(async_future.set_exception, e)
        
        request.future.add_done_callback(on_done)
        return async_future
    
    async def pending_count(self) -> int:
        """Get number of pending requests"""
        return self._queue.qsize()
    
    async def flush(self) -> None:
        """Trigger immediate processing"""
        pass
    
    async def _process_loop(self) -> None:
        """Main processing loop"""
        while self._running:
            try:
                batch = await self._drain_batch()
                if batch:
                    await self._process_batch(batch)
                else:
                    await asyncio.sleep(0.01)
            except asyncio.CancelledError:
                break
    
    async def _drain_batch(self) -> List[PipelinedRequest]:
        """Drain pending requests"""
        batch = []
        max_batch = self.config.auto_flush_threshold
        
        while len(batch) < max_batch:
            try:
                request = self._queue.get_nowait()
                batch.append(request)
            except asyncio.QueueEmpty:
                break
        
        return batch
    
    async def _process_batch(self, batch: List[PipelinedRequest]) -> None:
        """Process a batch"""
        self._in_flight += len(batch)
        
        try:
            for request in batch:
                try:
                    result = await self._execute_request(request)
                    request.future.set_result(result)
                except Exception as e:
                    request.future.set_exception(e)
        finally:
            self._in_flight -= len(batch)
    
    async def _execute_request(self, request: PipelinedRequest) -> Any:
        """Execute a single request"""
        # Async execution - implementation depends on async DB driver
        cursor = await self._connection.cursor()
        try:
            await cursor.execute(request.sql, request.params)
            
            if cursor.description:
                return await cursor.fetchall()
            else:
                return cursor.rowcount
        finally:
            await cursor.close()


class PipelineBuilder:
    """Builder for constructing query batches"""
    
    def __init__(self):
        self._queries: List[str] = []
    
    def add(self, sql: str) -> 'PipelineBuilder':
        """Add a query to the batch"""
        self._queries.append(sql)
        return self
    
    def build(self) -> List[str]:
        """Return the batch of queries"""
        return self._queries.copy()
