// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program ResourceResilienceTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  SysUtils,
  SBTelemetry,
  SBKeepalive,
  SBLeakDetector,
  ScratchBird.Client;

type
  TPingerProbe = class
  private
    FCalls: Integer;
    FHealthy: Boolean;
  public
    constructor Create(Healthy: Boolean = True);
    function Ping: Boolean;
    property Calls: Integer read FCalls;
  end;

  TNotificationSink = class
  public
    Calls: Integer;
    procedure Handle(const Notice: TNotification);
  end;

procedure Fail(const MessageText: string);
begin
  raise Exception.Create(MessageText);
end;

procedure AssertTrue(Value: Boolean; const MessageText: string);
begin
  if not Value then
    Fail(MessageText);
end;

procedure AssertFalse(Value: Boolean; const MessageText: string);
begin
  if Value then
    Fail(MessageText);
end;

procedure AssertEqualInt(Expected, Actual: Integer; const MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected=' + IntToStr(Expected) + ' actual=' + IntToStr(Actual));
end;

procedure AssertEqualString(const Expected, Actual, MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected="' + Expected + '" actual="' + Actual + '"');
end;

procedure AssertContains(const Needle, Haystack, MessageText: string);
begin
  if Pos(Needle, Haystack) = 0 then
    Fail(MessageText + ': missing "' + Needle + '" in "' + Haystack + '"');
end;

constructor TPingerProbe.Create(Healthy: Boolean);
begin
  inherited Create;
  FCalls := 0;
  FHealthy := Healthy;
end;

function TPingerProbe.Ping: Boolean;
begin
  Inc(FCalls);
  Result := FHealthy;
end;

procedure TNotificationSink.Handle(const Notice: TNotification);
begin
  Inc(Calls);
end;

procedure TestKeepaliveTrackerValidationWindow;
var
  Config: TKeepaliveConfig;
  Tracker: TKeepaliveTracker;
begin
  Config := DefaultKeepaliveConfig;
  Config.MaxIdleBeforeCheckMs := 50;

  Tracker := TKeepaliveTracker.Create(Config);
  try
    AssertFalse(Tracker.NeedsValidation, 'new tracker should not require validation');
    Sleep(80);
    AssertTrue(Tracker.NeedsValidation, 'idle tracker should require validation');
    Tracker.MarkActive;
    AssertFalse(Tracker.NeedsValidation, 'mark active should reset idle window');
  finally
    Tracker.Free;
  end;
end;

procedure TestKeepaliveManagerRegisterUnregisterAndPing;
var
  Config: TKeepaliveConfig;
  Manager: TKeepaliveManager;
  ProbeA, ProbeB: TPingerProbe;
  TrackerA, TrackerB: TKeepaliveTracker;
begin
  Config := DefaultKeepaliveConfig;
  Config.IntervalMs := 10;
  Config.MaxIdleBeforeCheckMs := 0;

  Manager := TKeepaliveManager.Create(Config);
  ProbeA := TPingerProbe.Create(True);
  ProbeB := TPingerProbe.Create(True);
  try
    TrackerA := Manager.Register('conn-1', ProbeA.Ping);
    AssertTrue(TrackerA <> nil, 'register should return tracker');
    AssertEqualInt(1, Manager.GetMonitoredCount, 'register should increase monitored count');

    TrackerB := Manager.Register('conn-1', ProbeB.Ping);
    AssertTrue(TrackerA = TrackerB, 'duplicate register should reuse tracker');
    AssertEqualInt(1, Manager.GetMonitoredCount, 'duplicate register should not duplicate entry count');

    Manager.Start;
    Sleep(80);
    AssertTrue(ProbeB.Calls > 0, 'manager should invoke replacement pinger for idle tracker');

    Manager.Unregister('conn-1');
    AssertEqualInt(0, Manager.GetMonitoredCount, 'unregister should remove monitored entry');
    Manager.Stop;
  finally
    ProbeB.Free;
    ProbeA.Free;
    Manager.Free;
  end;
end;

procedure TestCheckoutInfoCapturesMetadataPairs;
var
  Info: TCheckoutInfo;
begin
  Info := TCheckoutInfo.Create(False, ['driver', 'pascal', 'role', 'writer', 'tail']);
  try
    AssertEqualString('pascal', Info.Metadata.Values['driver'], 'metadata driver value');
    AssertEqualString('writer', Info.Metadata.Values['role'], 'metadata role value');
    AssertEqualString('tail', Info.Metadata.Values['meta_2'], 'odd metadata tail value');
  finally
    Info.Free;
  end;
end;

procedure TestLeakDetectorCheckoutCheckinAndReplace;
var
  Config: TLeakDetectionConfig;
  Detector: TLeakDetector;
begin
  Config := DefaultLeakDetectionConfig;
  Detector := TLeakDetector.Create(Config);
  try
    Detector.Checkout('conn-a', ['driver', 'pascal']);
    AssertEqualInt(1, Detector.GetActiveCount, 'first checkout should register active connection');

    Detector.Checkout('conn-a', ['driver', 'pascal-updated']);
    AssertEqualInt(1, Detector.GetActiveCount, 'duplicate checkout should replace entry');

    Detector.Checkout('conn-b', []);
    AssertEqualInt(2, Detector.GetActiveCount, 'second checkout should increase active count');

    Detector.Checkin('missing');
    AssertEqualInt(2, Detector.GetActiveCount, 'missing checkin should be ignored');

    Detector.Checkin('conn-a');
    AssertEqualInt(1, Detector.GetActiveCount, 'checkin should remove matching entry');

    Detector.Checkin('conn-b');
    AssertEqualInt(0, Detector.GetActiveCount, 'all checked-in entries should clear active count');
  finally
    Detector.Free;
  end;
end;

procedure TestLeakDetectorBackgroundLifecycle;
var
  Config: TLeakDetectionConfig;
  Detector: TLeakDetector;
begin
  Config := DefaultLeakDetectionConfig;
  Config.ThresholdMs := 0;
  Config.CheckIntervalMs := 5;

  Detector := TLeakDetector.Create(Config);
  try
    Detector.Start;
    Detector.Checkout('conn-z', ['driver', 'pascal']);
    Sleep(20);
    Detector.Checkin('conn-z');
    Detector.Stop;
    Detector.Stop;
    AssertEqualInt(0, Detector.GetActiveCount, 'background lifecycle should not leak checkout state');
  finally
    Detector.Free;
  end;
end;

procedure TestTelemetryCollectorJsonAndReset;
var
  Config: TTelemetryConfig;
  Collector: TTelemetryCollector;
  Span: TSpanContext;
  Summary, Slow, ResetSummary: string;
begin
  Config := DefaultTelemetryConfig;
  Config.SlowQueryThresholdMs := 0;
  Collector := TTelemetryCollector.Create(Config);
  try
    Span := Collector.StartSpan('query');
    Sleep(1);
    Collector.EndSpan(Span, True);

    Summary := Collector.ExportTelemetrySummaryJson;
    AssertContains('"total_invocations":1', Summary, 'telemetry summary invocation count');
    AssertContains('"operation":"query"', Summary, 'telemetry summary operation name');

    Slow := Collector.ExportSlowQueriesJson;
    AssertContains('"operation":"query"', Slow, 'slow query export operation');

    Collector.Reset;
    ResetSummary := Collector.ExportTelemetrySummaryJson;
    AssertContains('"total_invocations":0', ResetSummary, 'telemetry reset invocation count');
  finally
    Collector.Free;
  end;
end;

procedure TestClientEnterpriseSurfaceNoConnect;
var
  Client: TScratchBirdClient;
  Sink: TNotificationSink;
  ListenerId: UInt64;
  Diagnostics: string;
begin
  Client := TScratchBirdClient.Create;
  Sink := TNotificationSink.Create;
  try
    ListenerId := Client.AddNotificationListener(Sink.Handle);
    AssertTrue(ListenerId > 0, 'listener id should be assigned');
    AssertTrue(Client.RemoveNotificationListener(ListenerId), 'listener should be removable');
    AssertFalse(Client.RemoveNotificationListener(ListenerId), 'listener remove should be idempotent false');

    AssertEqualInt(0, Client.NotificationCount, 'notification queue starts empty');
    AssertEqualString('[]', Client.GetSlowOperationsJson, 'slow operations start empty');
    AssertContains('"total_invocations":0', Client.GetTelemetrySummaryJson, 'client telemetry starts at zero');

    Diagnostics := Client.GetDiagnosticsJson;
    AssertContains('"connected":false', Diagnostics, 'diagnostics connected state');
    AssertContains('"notification_queue_depth":0', Diagnostics, 'diagnostics queue depth');
  finally
    Sink.Free;
    Client.Free;
  end;
end;

begin
  try
    TestKeepaliveTrackerValidationWindow;
    TestKeepaliveManagerRegisterUnregisterAndPing;
    TestCheckoutInfoCapturesMetadataPairs;
    TestLeakDetectorCheckoutCheckinAndReplace;
    TestLeakDetectorBackgroundLifecycle;
    TestTelemetryCollectorJsonAndReset;
    TestClientEnterpriseSurfaceNoConnect;
    Writeln('ResourceResilienceTests: OK');
  except
    on E: Exception do
    begin
      Writeln('ResourceResilienceTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
