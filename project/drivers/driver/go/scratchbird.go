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
	"database/sql"
	"database/sql/driver"
)

func init() {
	sql.Register("scratchbird", &Driver{})
}

type Driver struct{}

func (d *Driver) Open(name string) (driver.Conn, error) {
	connector, err := d.OpenConnector(name)
	if err != nil {
		return nil, err
	}
	return connector.Connect(context.Background())
}

func (d *Driver) OpenConnector(name string) (driver.Connector, error) {
	cfg, err := ParseConfig(name)
	if err != nil {
		return nil, err
	}
	return &Connector{config: cfg}, nil
}

type Connector struct {
	config Config
}

func (c *Connector) Connect(ctx context.Context) (driver.Conn, error) {
	conn := &Conn{config: c.config}
	if err := conn.connect(ctx); err != nil {
		return nil, err
	}
	return conn, nil
}

func (c *Connector) ProbeAuthSurface(ctx context.Context) (AuthProbeResult, error) {
	conn := &Conn{config: c.config}
	return conn.probeAuthSurface(ctx)
}

func (c *Connector) Driver() driver.Driver {
	return &Driver{}
}
