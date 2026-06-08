// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"context"
	"database/sql/driver"
	"errors"
	"sync"
)

// PipelineConfig configures query pipelining
type PipelineConfig struct {
	MaxInFlight          int
	AutoFlush            bool
	AutoFlushThreshold   int
	FlushTimeoutMs       int
}

// DefaultPipelineConfig returns default pipeline configuration
func DefaultPipelineConfig() PipelineConfig {
	return PipelineConfig{
		MaxInFlight:        100,
		AutoFlush:          true,
		AutoFlushThreshold: 10,
		FlushTimeoutMs:     5000,
	}
}

// PipelinedRequest represents a queued query
type PipelinedRequest struct {
	SQL      string
	Params   []interface{}
	Response chan interface{}
	Error    chan error
}

// QueryPipeline batches multiple queries for efficiency
type QueryPipeline struct {
	config    PipelineConfig
	queue     chan *PipelinedRequest
	inFlight  int32
	running   bool
	conn      *Conn
	mu        sync.RWMutex
	wg        sync.WaitGroup
	ctx       context.Context
	cancel    context.CancelFunc
}

// NewQueryPipeline creates a new query pipeline
func NewQueryPipeline(config PipelineConfig) *QueryPipeline {
	ctx, cancel := context.WithCancel(context.Background())
	return &QueryPipeline{
		config: config,
		queue:  make(chan *PipelinedRequest, config.MaxInFlight),
		ctx:    ctx,
		cancel: cancel,
	}
}

// Start begins the pipeline processing
func (p *QueryPipeline) Start(conn *Conn) {
	p.mu.Lock()
	defer p.mu.Unlock()
	
	if p.running {
		return
	}
	
	p.conn = conn
	p.running = true
	
	// Start worker goroutines
	for i := 0; i < 4; i++ {
		p.wg.Add(1)
		go p.worker()
	}
}

// Stop halts the pipeline
func (p *QueryPipeline) Stop() {
	p.mu.Lock()
	if !p.running {
		p.mu.Unlock()
		return
	}
	p.running = false
	p.cancel()
	p.mu.Unlock()
	
	close(p.queue)
	p.wg.Wait()
}

// Queue adds a query to the pipeline
func (p *QueryPipeline) Queue(sql string, params ...interface{}) (<-chan interface{}, <-chan error) {
	respChan := make(chan interface{}, 1)
	errChan := make(chan error, 1)
	
	req := &PipelinedRequest{
		SQL:      sql,
		Params:   params,
		Response: respChan,
		Error:    errChan,
	}
	
	select {
	case p.queue <- req:
		// Successfully queued
	default:
		// Queue full
		errChan <- errors.New("pipeline at capacity")
		close(respChan)
		close(errChan)
	}
	
	return respChan, errChan
}

// PendingCount returns the number of pending requests
func (p *QueryPipeline) PendingCount() int {
	return len(p.queue)
}

// InFlightCount returns the number of in-flight requests
func (p *QueryPipeline) InFlightCount() int32 {
	p.mu.RLock()
	defer p.mu.RUnlock()
	return p.inFlight
}

// HasCapacity checks if pipeline has capacity
func (p *QueryPipeline) HasCapacity() bool {
	return p.InFlightCount() < int32(p.config.MaxInFlight)
}

// Flush triggers immediate processing
func (p *QueryPipeline) Flush() {
	// Process remaining items
	for len(p.queue) > 0 {
		// Let workers drain
	}
}

func (p *QueryPipeline) worker() {
	defer p.wg.Done()
	
	for {
		select {
		case <-p.ctx.Done():
			return
		case req, ok := <-p.queue:
			if !ok {
				return
			}
			p.processRequest(req)
		}
	}
}

func (p *QueryPipeline) processRequest(req *PipelinedRequest) {
	p.mu.Lock()
	p.inFlight++
	p.mu.Unlock()
	
	defer func() {
		p.mu.Lock()
		p.inFlight--
		p.mu.Unlock()
		close(req.Response)
		close(req.Error)
	}()
	
	// Execute the query
	stmt, err := p.conn.Prepare(req.SQL)
	if err != nil {
		req.Error <- err
		return
	}
	
	// Convert params to driver values
	params := make([]driver.Value, len(req.Params))
	for i, v := range req.Params {
		params[i] = driver.Value(v)
	}
	
	result, err := stmt.Query(params)
	if err != nil {
		req.Error <- err
		return
	}
	
	req.Response <- result
}

// PipelineBuilder helps construct batched queries
type PipelineBuilder struct {
	queries []string
}

// NewPipelineBuilder creates a new builder
func NewPipelineBuilder() *PipelineBuilder {
	return &PipelineBuilder{
		queries: make([]string, 0),
	}
}

// Add adds a query to the batch
func (b *PipelineBuilder) Add(sql string) *PipelineBuilder {
	b.queries = append(b.queries, sql)
	return b
}

// Build returns the batch
func (b *PipelineBuilder) Build() []string {
	return b.queries
}
