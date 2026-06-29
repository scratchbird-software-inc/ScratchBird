// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"encoding/binary"
	"errors"
	"io"
	"sort"
	"strconv"
	"strings"
)

const (
	protocolMagic  = 0x50574253 // Encodes bytes "SBWP" in little-endian frames.
	protocolMajor  = 1
	protocolMinor  = 1
	protocolVer    = (protocolMajor << 8) | protocolMinor
	headerSize     = 40
	maxMessageSize = 1024 * 1024 * 1024
)

const (
	connectValueText              = 1
	p1RowDescriptionHeaderBytes   = 72
	p1CanonicalTypeReferenceBytes = 144
)

const (
	authParamMethodID                = "auth_method_id"
	authParamMethodPayload           = "auth_method_payload"
	authParamPayloadJSON             = "auth_payload_json"
	authParamPayloadB64              = "auth_payload_b64"
	authParamProviderProfile         = "auth_provider_profile"
	authParamRequiredMethods         = "auth_required_methods"
	authParamForbiddenMethods        = "auth_forbidden_methods"
	authParamRequireChannelBinding   = "auth_require_channel_binding"
	authParamWorkloadIdentityToken   = "workload_identity_token"
	authParamProxyPrincipalAssertion = "proxy_principal_assertion"
)

type AuthPluginSelection struct {
	MethodID                string
	MethodPayload           string
	PayloadJSON             string
	PayloadB64              string
	ProviderProfile         string
	RequiredMethods         string
	ForbiddenMethods        string
	RequireChannelBinding   bool
	WorkloadIdentityToken   string
	ProxyPrincipalAssertion string
}

func ApplyAuthPluginSelection(params map[string]string, selection AuthPluginSelection) error {
	if params == nil {
		return errors.New("params map is nil")
	}
	methodID := strings.TrimSpace(selection.MethodID)
	if methodID != "" && !strings.HasPrefix(methodID, "scratchbird.auth.") {
		return errors.New("invalid auth_method_id namespace")
	}
	if methodID != "" {
		params[authParamMethodID] = methodID
	}
	if selection.MethodPayload != "" {
		params[authParamMethodPayload] = selection.MethodPayload
	}
	if selection.PayloadJSON != "" {
		params[authParamPayloadJSON] = selection.PayloadJSON
	}
	if selection.PayloadB64 != "" {
		params[authParamPayloadB64] = selection.PayloadB64
	}
	if selection.ProviderProfile != "" {
		params[authParamProviderProfile] = selection.ProviderProfile
	}
	if selection.RequiredMethods != "" {
		params[authParamRequiredMethods] = selection.RequiredMethods
	}
	if selection.ForbiddenMethods != "" {
		params[authParamForbiddenMethods] = selection.ForbiddenMethods
	}
	if selection.RequireChannelBinding {
		params[authParamRequireChannelBinding] = "1"
	}
	if selection.WorkloadIdentityToken != "" {
		params[authParamWorkloadIdentityToken] = selection.WorkloadIdentityToken
	}
	if selection.ProxyPrincipalAssertion != "" {
		params[authParamProxyPrincipalAssertion] = selection.ProxyPrincipalAssertion
	}
	return nil
}

type messageType byte

const (
	msgStartup        messageType = 0x01
	msgAuthResponse   messageType = 0x02
	msgQuery          messageType = 0x03
	msgParse          messageType = 0x04
	msgBind           messageType = 0x05
	msgDescribe       messageType = 0x06
	msgExecute        messageType = 0x07
	msgClose          messageType = 0x08
	msgSync           messageType = 0x09
	msgFlush          messageType = 0x0A
	msgCancel         messageType = 0x0B
	msgTerminate      messageType = 0x0C
	msgCopyData       messageType = 0x0D
	msgCopyDone       messageType = 0x0E
	msgCopyFail       messageType = 0x0F
	msgSblrExecute    messageType = 0x10
	msgSubscribe      messageType = 0x11
	msgUnsubscribe    messageType = 0x12
	msgFederatedQuery messageType = 0x13
	msgStreamControl  messageType = 0x14
	msgTxnBegin       messageType = 0x15
	msgTxnCommit      messageType = 0x16
	msgTxnRollback    messageType = 0x17
	msgTxnSavepoint   messageType = 0x18
	msgTxnRelease     messageType = 0x19
	msgTxnRollbackTo  messageType = 0x1A
	msgPing           messageType = 0x1B
	msgSetOption      messageType = 0x1C
	msgClusterAuth    messageType = 0x1D
	msgAttachCreate   messageType = 0x1E
	msgAttachDetach   messageType = 0x1F
	msgAttachList     messageType = 0x20

	msgAuthRequest          messageType = 0x40
	msgAuthOk               messageType = 0x41
	msgAuthContinue         messageType = 0x42
	msgReady                messageType = 0x43
	msgRowDescription       messageType = 0x44
	msgDataRow              messageType = 0x45
	msgCommandComplete      messageType = 0x46
	msgEmptyQuery           messageType = 0x47
	msgError                messageType = 0x48
	msgNotice               messageType = 0x49
	msgParseComplete        messageType = 0x4A
	msgBindComplete         messageType = 0x4B
	msgCloseComplete        messageType = 0x4C
	msgPortalSuspended      messageType = 0x4D
	msgNoData               messageType = 0x4E
	msgParameterStatus      messageType = 0x4F
	msgParameterDescription messageType = 0x50
	msgCopyInResponse       messageType = 0x51
	msgCopyOutResponse      messageType = 0x52
	msgCopyBothResponse     messageType = 0x53
	msgNotification         messageType = 0x54
	msgFunctionResult       messageType = 0x55
	msgNegotiateVersion     messageType = 0x56
	msgSblrCompiled         messageType = 0x57
	msgQueryPlan            messageType = 0x58
	msgStreamReady          messageType = 0x59
	msgStreamData           messageType = 0x5A
	msgStreamEnd            messageType = 0x5B
	msgTxnStatus            messageType = 0x5C
	msgPong                 messageType = 0x5D
	msgClusterAuthOk        messageType = 0x5E
	msgFederatedResult      messageType = 0x5F
	msgHeartbeat            messageType = 0x80
	msgExtension            messageType = 0x81
)

const (
	msgFlagCompressed = 0x01
	msgFlagContinued  = 0x02
	msgFlagFinal      = 0x04
	msgFlagUrgent     = 0x08
	msgFlagEncrypted  = 0x10
	msgFlagChecksum   = 0x20
)

const (
	featureCompression   uint64 = 1 << 0
	featureStreaming     uint64 = 1 << 1
	featureSBLR          uint64 = 1 << 2
	featureFederation    uint64 = 1 << 3
	featureNotifications uint64 = 1 << 4
	featureQueryPlan     uint64 = 1 << 5
	featureBatch         uint64 = 1 << 6
	featurePipeline      uint64 = 1 << 7
	featureBinaryCopy    uint64 = 1 << 8
	featureSavepoints    uint64 = 1 << 9
	feature2PC           uint64 = 1 << 10
	featureChecksums     uint64 = 1 << 11
)

const (
	queryFlagDescribeOnly uint32 = 0x01
	queryFlagNoPortal     uint32 = 0x02
	queryFlagBinaryResult uint32 = 0x04
	queryFlagIncludePlan  uint32 = 0x08
	queryFlagReturnSblr   uint32 = 0x10
	queryFlagNoCache      uint32 = 0x20
)

// COPY format codes
const (
	CopyFormatText   = 0
	CopyFormatBinary = 1
)

const (
	isolationReadUncommitted byte = 0
	isolationReadCommitted   byte = 1
	isolationRepeatableRead  byte = 2
	isolationSerializable    byte = 3
)

const (
	readCommittedModeDefault         byte = 0
	readCommittedModeReadConsistency byte = 1
	readCommittedModeRecordVersion   byte = 2
	readCommittedModeNoRecordVersion byte = 3
)

const (
	txnFlagHasIsolation         uint16 = 0x0001
	txnFlagHasAccess            uint16 = 0x0002
	txnFlagHasDeferrable        uint16 = 0x0004
	txnFlagHasWait              uint16 = 0x0008
	txnFlagHasTimeout           uint16 = 0x0010
	txnFlagHasAutocommit        uint16 = 0x0020
	txnFlagHasReadCommittedMode uint16 = 0x0100
)

const (
	streamStart  byte = 0
	streamPause  byte = 1
	streamResume byte = 2
	streamCancel byte = 3
	streamAck    byte = 4
)

const (
	subTypeChannel byte = 0
	subTypeTable   byte = 1
	subTypeQuery   byte = 2
	subTypeEvent   byte = 3
)

type authMethod byte

const (
	authOK          authMethod = 0
	authPassword    authMethod = 1
	authMD5         authMethod = 2
	authScramSha256 authMethod = 3
	authScramSha512 authMethod = 4
	authToken       authMethod = 5
	authPeer        authMethod = 6
	authReattach    authMethod = 7
	authCertificate authMethod = 8
	authGSSAPI      authMethod = 9
	authSSPI        authMethod = 10
	authLDAP        authMethod = 11
	authSAML        authMethod = 12
	authOIDC        authMethod = 13
	authMFATOTP     authMethod = 14
	authClusterPKI  authMethod = 15
)

type messageHeader struct {
	typ          messageType
	flags        byte
	length       uint32
	sequence     uint32
	attachmentID [16]byte
	txnID        uint64
}

type protocolMessage struct {
	header messageHeader
	body   []byte
}

type columnInfo struct {
	name         string
	tableOID     uint32
	columnIndex  uint16
	typeOID      uint32
	typeSize     int16
	typeModifier int32
	format       uint8
	nullable     bool
}

type columnValue struct {
	data []byte
	null bool
}

func encodeMessage(header messageHeader, payload []byte) []byte {
	buf := make([]byte, headerSize+len(payload))
	copy(buf[0:4], []byte("SBWP"))
	buf[4] = protocolMajor
	buf[5] = protocolMinor
	buf[6] = byte(header.typ)
	buf[7] = header.flags
	binary.LittleEndian.PutUint32(buf[8:12], uint32(len(payload)))
	binary.LittleEndian.PutUint32(buf[12:16], header.sequence)
	copy(buf[16:32], header.attachmentID[:])
	binary.LittleEndian.PutUint64(buf[32:40], header.txnID)
	copy(buf[40:], payload)
	return buf
}

func decodeHeader(header []byte) (messageHeader, error) {
	if len(header) != headerSize {
		return messageHeader{}, errors.New("invalid header length")
	}
	if string(header[0:4]) != "SBWP" {
		return messageHeader{}, errors.New("invalid protocol magic")
	}
	major := header[4]
	minor := header[5]
	if major != protocolMajor || minor != protocolMinor {
		return messageHeader{}, errors.New("unsupported protocol version")
	}
	length := binary.LittleEndian.Uint32(header[8:12])
	if length > maxMessageSize {
		return messageHeader{}, errors.New("payload too large")
	}
	var attachment [16]byte
	copy(attachment[:], header[16:32])
	return messageHeader{
		typ:          messageType(header[6]),
		flags:        header[7],
		length:       length,
		sequence:     binary.LittleEndian.Uint32(header[12:16]),
		attachmentID: attachment,
		txnID:        binary.LittleEndian.Uint64(header[32:40]),
	}, nil
}

func readMessage(r io.Reader) (protocolMessage, error) {
	headerBytes := make([]byte, headerSize)
	if _, err := io.ReadFull(r, headerBytes); err != nil {
		return protocolMessage{}, err
	}
	header, err := decodeHeader(headerBytes)
	if err != nil {
		return protocolMessage{}, err
	}
	body := make([]byte, header.length)
	if header.length > 0 {
		if _, err := io.ReadFull(r, body); err != nil {
			return protocolMessage{}, err
		}
	}
	return protocolMessage{header: header, body: body}, nil
}

func buildStartupPayload(features uint64, params map[string]string) []byte {
	paramBytes := buildP1ParamList(params)
	payload := make([]byte, 88+len(paramBytes))
	offset := 0
	binary.LittleEndian.PutUint16(payload[offset:offset+2], uint16(protocolVer))
	offset += 2
	binary.LittleEndian.PutUint16(payload[offset:offset+2], uint16(protocolVer))
	offset += 2
	binary.LittleEndian.PutUint32(payload[offset:offset+4], 0)
	offset += 4
	binary.LittleEndian.PutUint64(payload[offset:offset+8], features)
	offset += 8
	binary.LittleEndian.PutUint64(payload[offset:offset+8], 0)
	offset += 8
	binary.LittleEndian.PutUint64(payload[offset:offset+8], 0)
	offset += 8
	offset += 16 * 3
	binary.LittleEndian.PutUint32(payload[offset:offset+4], uint32(len(params)))
	offset += 4
	copy(payload[offset:offset+len(paramBytes)], paramBytes)
	offset += len(paramBytes)
	binary.LittleEndian.PutUint32(payload[offset:offset+4], 0)
	return payload
}

func buildP1ParamList(params map[string]string) []byte {
	buf := make([]byte, 0, 128)
	keys := make([]string, 0, len(params))
	for key := range params {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		value := params[key]
		buf = appendP1LengthPrefixedString(buf, key)
		tail := make([]byte, 2+4)
		binary.LittleEndian.PutUint16(tail[0:2], connectValueText)
		valueBytes := []byte(value)
		binary.LittleEndian.PutUint32(tail[2:6], uint32(len(valueBytes)))
		buf = append(buf, tail...)
		buf = append(buf, valueBytes...)
	}
	return buf
}

func appendP1LengthPrefixedString(buf []byte, value string) []byte {
	valueBytes := []byte(value)
	lenBytes := make([]byte, 4)
	binary.LittleEndian.PutUint32(lenBytes, uint32(len(valueBytes)))
	buf = append(buf, lenBytes...)
	buf = append(buf, valueBytes...)
	return buf
}

func buildParamList(params map[string]string) []byte {
	buf := make([]byte, 0, 128)
	for key, value := range params {
		buf = append(buf, []byte(key)...)
		buf = append(buf, 0)
		buf = append(buf, []byte(value)...)
		buf = append(buf, 0)
	}
	buf = append(buf, 0)
	return buf
}

func parseAuthRequest(payload []byte) (authMethod, []byte, error) {
	if len(payload) < 4 {
		return 0, nil, errors.New("auth request truncated")
	}
	method := authMethod(payload[0])
	return method, append([]byte{}, payload[4:]...), nil
}

func parseAuthContinue(payload []byte) (authMethod, byte, []byte, error) {
	if len(payload) < 8 {
		return 0, 0, nil, errors.New("auth continue truncated")
	}
	method := authMethod(payload[0])
	stage := payload[1]
	dataLen := binary.LittleEndian.Uint32(payload[4:8])
	if int(8+dataLen) > len(payload) {
		return 0, 0, nil, errors.New("auth continue truncated")
	}
	data := append([]byte{}, payload[8:8+dataLen]...)
	return method, stage, data, nil
}

func parseAuthOk(payload []byte) ([]byte, []byte, error) {
	if len(payload) < 16+4 {
		return nil, nil, errors.New("auth ok truncated")
	}
	sessionID := append([]byte{}, payload[:16]...)
	infoLen := binary.LittleEndian.Uint32(payload[16:20])
	if int(20+infoLen) > len(payload) {
		return nil, nil, errors.New("auth ok truncated")
	}
	info := append([]byte{}, payload[20:20+infoLen]...)
	return sessionID, info, nil
}

func buildQueryPayload(query string, flags uint32, maxRows uint32, timeoutMs uint32) []byte {
	queryBytes := append([]byte(query), 0)
	payload := make([]byte, 4+4+4+len(queryBytes))
	binary.LittleEndian.PutUint32(payload[0:4], flags)
	binary.LittleEndian.PutUint32(payload[4:8], maxRows)
	binary.LittleEndian.PutUint32(payload[8:12], timeoutMs)
	copy(payload[12:], queryBytes)
	return payload
}

func buildParsePayload(statementName, query string, paramTypes []uint32) []byte {
	nameBytes := []byte(statementName)
	queryBytes := []byte(query)
	payloadLen := 4 + len(nameBytes) + 4 + len(queryBytes) + 2 + 2 + len(paramTypes)*4
	payload := make([]byte, payloadLen)
	offset := 0
	binary.LittleEndian.PutUint32(payload[offset:offset+4], uint32(len(nameBytes)))
	offset += 4
	copy(payload[offset:offset+len(nameBytes)], nameBytes)
	offset += len(nameBytes)
	binary.LittleEndian.PutUint32(payload[offset:offset+4], uint32(len(queryBytes)))
	offset += 4
	copy(payload[offset:offset+len(queryBytes)], queryBytes)
	offset += len(queryBytes)
	binary.LittleEndian.PutUint16(payload[offset:offset+2], uint16(len(paramTypes)))
	offset += 2
	binary.LittleEndian.PutUint16(payload[offset:offset+2], 0)
	offset += 2
	for _, oid := range paramTypes {
		binary.LittleEndian.PutUint32(payload[offset:offset+4], oid)
		offset += 4
	}
	return payload
}

type paramValue struct {
	format uint16
	data   []byte
	null   bool
}

func buildBindPayload(portalName, statementName string, params []paramValue, resultFormats []uint16) []byte {
	portalBytes := []byte(portalName)
	stmtBytes := []byte(statementName)
	paramFormats := make([]uint16, len(params))
	for i, param := range params {
		paramFormats[i] = param.format
	}
	payloadLen := 4 + len(portalBytes) + 4 + len(stmtBytes)
	payloadLen += 2 + len(paramFormats)*2
	payloadLen += 2 + 2
	for _, param := range params {
		payloadLen += 4
		if !param.null {
			payloadLen += len(param.data)
		}
	}
	payloadLen += 2 + len(resultFormats)*2

	payload := make([]byte, payloadLen)
	offset := 0
	binary.LittleEndian.PutUint32(payload[offset:offset+4], uint32(len(portalBytes)))
	offset += 4
	copy(payload[offset:offset+len(portalBytes)], portalBytes)
	offset += len(portalBytes)
	binary.LittleEndian.PutUint32(payload[offset:offset+4], uint32(len(stmtBytes)))
	offset += 4
	copy(payload[offset:offset+len(stmtBytes)], stmtBytes)
	offset += len(stmtBytes)
	binary.LittleEndian.PutUint16(payload[offset:offset+2], uint16(len(paramFormats)))
	offset += 2
	for _, fmtCode := range paramFormats {
		binary.LittleEndian.PutUint16(payload[offset:offset+2], fmtCode)
		offset += 2
	}
	binary.LittleEndian.PutUint16(payload[offset:offset+2], uint16(len(params)))
	offset += 2
	binary.LittleEndian.PutUint16(payload[offset:offset+2], 0)
	offset += 2
	for _, param := range params {
		if param.null {
			binary.LittleEndian.PutUint32(payload[offset:offset+4], ^uint32(0))
			offset += 4
			continue
		}
		binary.LittleEndian.PutUint32(payload[offset:offset+4], uint32(len(param.data)))
		offset += 4
		copy(payload[offset:offset+len(param.data)], param.data)
		offset += len(param.data)
	}
	binary.LittleEndian.PutUint16(payload[offset:offset+2], uint16(len(resultFormats)))
	offset += 2
	for _, fmtCode := range resultFormats {
		binary.LittleEndian.PutUint16(payload[offset:offset+2], fmtCode)
		offset += 2
	}
	return payload
}

func buildDescribePayload(describeType byte, name string) []byte {
	nameBytes := []byte(name)
	payload := make([]byte, 4+4+len(nameBytes))
	payload[0] = describeType
	binary.LittleEndian.PutUint32(payload[4:8], uint32(len(nameBytes)))
	copy(payload[8:], nameBytes)
	return payload
}

func buildExecutePayload(portalName string, maxRows uint32) []byte {
	portalBytes := []byte(portalName)
	payload := make([]byte, 4+len(portalBytes)+4)
	binary.LittleEndian.PutUint32(payload[0:4], uint32(len(portalBytes)))
	copy(payload[4:4+len(portalBytes)], portalBytes)
	binary.LittleEndian.PutUint32(payload[4+len(portalBytes):], maxRows)
	return payload
}

func buildSblrExecutePayload(sblrHash uint64, bytecode []byte, params []paramValue) []byte {
	payloadLen := 8 + 4 + 2 + 2 + len(bytecode)
	for _, param := range params {
		payloadLen += 4
		if !param.null {
			payloadLen += len(param.data)
		}
	}
	payload := make([]byte, payloadLen)
	offset := 0
	binary.LittleEndian.PutUint64(payload[offset:offset+8], sblrHash)
	offset += 8
	binary.LittleEndian.PutUint32(payload[offset:offset+4], uint32(len(bytecode)))
	offset += 4
	binary.LittleEndian.PutUint16(payload[offset:offset+2], uint16(len(params)))
	offset += 2
	binary.LittleEndian.PutUint16(payload[offset:offset+2], 0)
	offset += 2
	copy(payload[offset:offset+len(bytecode)], bytecode)
	offset += len(bytecode)
	for _, param := range params {
		if param.null {
			binary.LittleEndian.PutUint32(payload[offset:offset+4], 0xFFFFFFFF)
			offset += 4
			continue
		}
		binary.LittleEndian.PutUint32(payload[offset:offset+4], uint32(len(param.data)))
		offset += 4
		copy(payload[offset:offset+len(param.data)], param.data)
		offset += len(param.data)
	}
	return payload
}

func buildSubscribePayload(subscribeType uint8, channel, filter string) []byte {
	channelBytes := []byte(channel)
	filterBytes := []byte(filter)
	payloadLen := 4 + 4 + len(channelBytes) + 4 + len(filterBytes)
	payload := make([]byte, payloadLen)
	payload[0] = subscribeType
	offset := 4
	binary.LittleEndian.PutUint32(payload[offset:offset+4], uint32(len(channelBytes)))
	offset += 4
	copy(payload[offset:offset+len(channelBytes)], channelBytes)
	offset += len(channelBytes)
	binary.LittleEndian.PutUint32(payload[offset:offset+4], uint32(len(filterBytes)))
	offset += 4
	copy(payload[offset:offset+len(filterBytes)], filterBytes)
	return payload
}

func buildUnsubscribePayload(channel string) []byte {
	channelBytes := []byte(channel)
	payload := make([]byte, 4+len(channelBytes))
	binary.LittleEndian.PutUint32(payload[0:4], uint32(len(channelBytes)))
	copy(payload[4:], channelBytes)
	return payload
}

func buildTxnBeginPayload(flags uint16, conflictAction uint8, autocommitMode uint8, isolationLevel uint8, accessMode uint8, deferrable uint8, waitMode uint8, timeoutMs uint32, readCommittedMode uint8) []byte {
	payloadLen := 2 + 1 + 1 + 1 + 1 + 1 + 1 + 4
	if flags&txnFlagHasReadCommittedMode != 0 {
		payloadLen += 4
	}
	payload := make([]byte, payloadLen)
	binary.LittleEndian.PutUint16(payload[0:2], flags)
	payload[2] = conflictAction
	payload[3] = autocommitMode
	payload[4] = isolationLevel
	payload[5] = accessMode
	payload[6] = deferrable
	payload[7] = waitMode
	binary.LittleEndian.PutUint32(payload[8:12], timeoutMs)
	if flags&txnFlagHasReadCommittedMode != 0 {
		payload[12] = readCommittedMode
	}
	return payload
}

func buildTxnCommitPayload(flags uint8) []byte {
	payload := make([]byte, 4)
	payload[0] = flags
	return payload
}

func buildTxnRollbackPayload(flags uint8) []byte {
	payload := make([]byte, 4)
	payload[0] = flags
	return payload
}

func buildTxnSavepointPayload(name string) []byte {
	nameBytes := []byte(name)
	payload := make([]byte, 4+len(nameBytes))
	binary.LittleEndian.PutUint32(payload[0:4], uint32(len(nameBytes)))
	copy(payload[4:], nameBytes)
	return payload
}

func buildTxnReleasePayload(name string) []byte {
	return buildTxnSavepointPayload(name)
}

func buildTxnRollbackToPayload(name string) []byte {
	return buildTxnSavepointPayload(name)
}

func buildSetOptionPayload(name, value string) []byte {
	nameBytes := []byte(name)
	valueBytes := []byte(value)
	payload := make([]byte, 4+len(nameBytes)+4+len(valueBytes))
	binary.LittleEndian.PutUint32(payload[0:4], uint32(len(nameBytes)))
	copy(payload[4:4+len(nameBytes)], nameBytes)
	offset := 4 + len(nameBytes)
	binary.LittleEndian.PutUint32(payload[offset:offset+4], uint32(len(valueBytes)))
	copy(payload[offset+4:], valueBytes)
	return payload
}

func buildStreamControlPayload(controlType uint8, windowSize uint32, timeoutMs uint32) []byte {
	payload := make([]byte, 8+4)
	payload[0] = controlType
	binary.LittleEndian.PutUint32(payload[4:8], windowSize)
	binary.LittleEndian.PutUint32(payload[8:12], timeoutMs)
	return payload
}

func buildAttachCreatePayload(mode, dbName string) []byte {
	modeBytes := []byte(mode)
	dbBytes := []byte(dbName)
	payload := make([]byte, 4+len(modeBytes)+4+len(dbBytes))
	binary.LittleEndian.PutUint32(payload[0:4], uint32(len(modeBytes)))
	copy(payload[4:4+len(modeBytes)], modeBytes)
	offset := 4 + len(modeBytes)
	binary.LittleEndian.PutUint32(payload[offset:offset+4], uint32(len(dbBytes)))
	copy(payload[offset+4:], dbBytes)
	return payload
}

func buildClosePayload(closeType byte, name string) []byte {
	nameBytes := []byte(name)
	payload := make([]byte, 4+4+len(nameBytes))
	payload[0] = closeType
	binary.LittleEndian.PutUint32(payload[4:8], uint32(len(nameBytes)))
	copy(payload[8:], nameBytes)
	return payload
}

func buildCancelPayload(cancelType uint32, targetSeq uint32) []byte {
	payload := make([]byte, 8)
	binary.LittleEndian.PutUint32(payload[0:4], cancelType)
	binary.LittleEndian.PutUint32(payload[4:8], targetSeq)
	return payload
}

func parseReady(payload []byte) (byte, uint64, uint64, error) {
	if len(payload) >= 76 {
		switch payload[56] {
		case 'I', 'T', 'E', 'R', 'A':
			txnID := binary.LittleEndian.Uint64(payload[48:56])
			status := byte(0)
			if payload[56] == 'T' || payload[56] == 'E' {
				status = 1
			}
			return status, txnID, txnID, nil
		}
	}
	if len(payload) < 1+3+8+8 {
		return 0, 0, 0, errors.New("ready truncated")
	}
	status := payload[0]
	txnID := binary.LittleEndian.Uint64(payload[4:12])
	epoch := binary.LittleEndian.Uint64(payload[12:20])
	return status, txnID, epoch, nil
}

func parseTxnStatus(payload []byte) (byte, uint64, error) {
	if len(payload) < 12 {
		return 0, 0, errors.New("txn status truncated")
	}
	status := payload[0]
	txnID := binary.LittleEndian.Uint64(payload[4:12])
	return status, txnID, nil
}

type parameterStatus struct {
	name  string
	value string
}

func parseParameterStatuses(payload []byte) ([]parameterStatus, error) {
	if len(payload) < 8 {
		return nil, errors.New("parameter status truncated")
	}
	count := int(int32(binary.LittleEndian.Uint32(payload[0:4])))
	if count > 0 && count <= 256 {
		offset := 4
		statuses := make([]parameterStatus, 0, count)
		ok := true
		for i := 0; i < count; i++ {
			if offset+4 > len(payload) {
				ok = false
				break
			}
			nameLen := int(int32(binary.LittleEndian.Uint32(payload[offset : offset+4])))
			offset += 4
			if nameLen < 0 || offset+nameLen+7 > len(payload) {
				ok = false
				break
			}
			name := string(payload[offset : offset+nameLen])
			offset += nameLen
			offset += 3
			valueLen := int(int32(binary.LittleEndian.Uint32(payload[offset : offset+4])))
			offset += 4
			if valueLen < 0 || offset+valueLen > len(payload) {
				ok = false
				break
			}
			value := string(payload[offset : offset+valueLen])
			offset += valueLen
			statuses = append(statuses, parameterStatus{name: name, value: value})
		}
		if ok && offset == len(payload) {
			return statuses, nil
		}
	}

	offset := 0
	nameLen := int(binary.LittleEndian.Uint32(payload[offset : offset+4]))
	offset += 4
	if offset+nameLen+4 > len(payload) {
		return nil, errors.New("parameter status truncated")
	}
	name := string(payload[offset : offset+nameLen])
	offset += nameLen
	valueLen := int(binary.LittleEndian.Uint32(payload[offset : offset+4]))
	offset += 4
	if offset+valueLen > len(payload) {
		return nil, errors.New("parameter status truncated")
	}
	value := string(payload[offset : offset+valueLen])
	return []parameterStatus{{name: name, value: value}}, nil
}

func parseParameterStatus(payload []byte) (string, string, error) {
	statuses, err := parseParameterStatuses(payload)
	if err != nil {
		return "", "", err
	}
	if len(statuses) == 0 {
		return "", "", errors.New("parameter status truncated")
	}
	return statuses[0].name, statuses[0].value, nil
}

func parseParameterDescription(payload []byte) ([]uint32, error) {
	if len(payload) >= p1RowDescriptionHeaderBytes &&
		binary.LittleEndian.Uint16(payload[0:2]) == 1 &&
		payload[3] == 1 {
		count := int(binary.LittleEndian.Uint32(payload[68:72]))
		pos := p1RowDescriptionHeaderBytes
		types := make([]uint32, 0, count)
		for i := 0; i < count; i++ {
			if pos+4+4+8+8+p1CanonicalTypeReferenceBytes+4+5 > len(payload) {
				return nil, errors.New("P1 parameter description truncated")
			}
			typeOffset := pos + 4 + 4 + 8 + 8
			types = append(types, oidFromCanonicalTypeRef(payload, typeOffset))
			pos = typeOffset + p1CanonicalTypeReferenceBytes + 4
			_, next, err := readNullableText(payload, pos)
			if err != nil {
				return nil, err
			}
			pos = next
		}
		return types, nil
	}
	if len(payload) < 4 {
		return nil, errors.New("parameter description truncated")
	}
	num := int(binary.LittleEndian.Uint16(payload[:2]))
	pos := 4
	types := make([]uint32, 0, num)
	for i := 0; i < num; i++ {
		if len(payload) < pos+4 {
			return nil, errors.New("parameter description truncated")
		}
		types = append(types, binary.LittleEndian.Uint32(payload[pos:pos+4]))
		pos += 4
	}
	return types, nil
}

func parseRowDescription(payload []byte) ([]columnInfo, error) {
	if isP1RowDescription(payload) {
		return parseP1RowDescription(payload)
	}
	if len(payload) < 4 {
		return nil, errors.New("row description truncated")
	}
	offset := 0
	count := int(binary.LittleEndian.Uint16(payload[offset : offset+2]))
	offset += 4
	cols := make([]columnInfo, 0, count)
	for i := 0; i < count; i++ {
		if offset+4 > len(payload) {
			return nil, errors.New("row description truncated")
		}
		nameLen := int(binary.LittleEndian.Uint32(payload[offset : offset+4]))
		offset += 4
		if offset+nameLen+4+2+4+2+4+1+1+2 > len(payload) {
			return nil, errors.New("row description truncated")
		}
		name := string(payload[offset : offset+nameLen])
		offset += nameLen
		tableOID := binary.LittleEndian.Uint32(payload[offset : offset+4])
		offset += 4
		columnIndex := binary.LittleEndian.Uint16(payload[offset : offset+2])
		offset += 2
		typeOID := binary.LittleEndian.Uint32(payload[offset : offset+4])
		offset += 4
		typeSize := int16(binary.LittleEndian.Uint16(payload[offset : offset+2]))
		offset += 2
		typeModifier := int32(binary.LittleEndian.Uint32(payload[offset : offset+4]))
		offset += 4
		format := payload[offset]
		offset++
		nullable := payload[offset] == 1
		offset++
		offset += 2
		cols = append(cols, columnInfo{
			name:         name,
			tableOID:     tableOID,
			columnIndex:  columnIndex,
			typeOID:      typeOID,
			typeSize:     typeSize,
			typeModifier: typeModifier,
			format:       format,
			nullable:     nullable,
		})
	}
	return cols, nil
}

func isP1RowDescription(payload []byte) bool {
	return len(payload) >= p1RowDescriptionHeaderBytes &&
		binary.LittleEndian.Uint16(payload[0:2]) == 1 &&
		payload[3] == 1
}

func parseP1RowDescription(payload []byte) ([]columnInfo, error) {
	count := int(int32(binary.LittleEndian.Uint32(payload[4:8])))
	if count < 0 {
		return nil, errors.New("P1 row description column count invalid")
	}
	offset := p1RowDescriptionHeaderBytes
	cols := make([]columnInfo, 0, count)
	for i := 0; i < count; i++ {
		fixedColumnBytes := 4 + 4 + 8 + p1CanonicalTypeReferenceBytes + 56
		if offset+fixedColumnBytes > len(payload) {
			return nil, errors.New("P1 row description truncated")
		}
		ordinal := int(int32(binary.LittleEndian.Uint32(payload[offset : offset+4])))
		offset += 4
		offset++
		format := uint8(formatBinary)
		if payload[offset] == 1 {
			format = uint8(formatText)
		}
		offset++
		nullable := payload[offset] == 1
		offset++
		offset++
		offset += 8
		typeOID := oidFromCanonicalTypeRef(payload, offset)
		offset += p1CanonicalTypeReferenceBytes
		offset += 16 * 3
		offset += 4
		offset += 2
		offset += 2
		name, nextOffset, err := readNullableText(payload, offset)
		if err != nil {
			return nil, err
		}
		offset = nextOffset
		if name == "" {
			name = "column" + strconv.Itoa(i+1)
		}
		columnIndex := uint16(i)
		if ordinal > 0 {
			columnIndex = uint16(ordinal - 1)
		}
		cols = append(cols, columnInfo{
			name:         name,
			tableOID:     0,
			columnIndex:  columnIndex,
			typeOID:      typeOID,
			typeSize:     typeSizeForOID(typeOID),
			typeModifier: -1,
			format:       format,
			nullable:     nullable,
		})
	}
	return cols, nil
}

func oidFromCanonicalTypeRef(payload []byte, offset int) uint32 {
	if offset+4 > len(payload) {
		return oidText
	}
	family := binary.LittleEndian.Uint16(payload[offset : offset+2])
	code := binary.LittleEndian.Uint16(payload[offset+2 : offset+4])
	switch {
	case family == 1 && code == 1:
		return oidBool
	case family == 2 && code == 3:
		return oidInt4
	case family == 2 && code == 4:
		return oidInt8
	case family == 4 && code == 1:
		return oidNumeric
	case family == 6 && code == 2:
		return oidFloat8
	case family == 8 && code == 1:
		return oidText
	default:
		return oidText
	}
}

func typeSizeForOID(oid uint32) int16 {
	switch oid {
	case oidBool:
		return 1
	case oidInt4:
		return 4
	case oidInt8, oidFloat8:
		return 8
	default:
		return -1
	}
}

func readNullableText(payload []byte, offset int) (string, int, error) {
	if offset+5 > len(payload) {
		return "", offset, errors.New("nullable text truncated")
	}
	tag := payload[offset]
	offset++
	length := int(int32(binary.LittleEndian.Uint32(payload[offset : offset+4])))
	offset += 4
	if length < 0 {
		return "", offset, errors.New("nullable text length invalid")
	}
	if tag == 0 {
		return "", offset, nil
	}
	if offset+length > len(payload) {
		return "", offset, errors.New("nullable text truncated")
	}
	return string(payload[offset : offset+length]), offset + length, nil
}

func parseDataRow(payload []byte, columnCount int) ([]columnValue, error) {
	if len(payload) < 4 {
		return nil, errors.New("row data truncated")
	}
	offset := 0
	count := int(binary.LittleEndian.Uint16(payload[offset : offset+2]))
	offset += 2
	nullBytes := int(binary.LittleEndian.Uint16(payload[offset : offset+2]))
	offset += 2
	if count != columnCount {
		return nil, errors.New("row data column count mismatch")
	}
	if offset+nullBytes > len(payload) {
		return nil, errors.New("row data truncated")
	}
	nullBitmap := payload[offset : offset+nullBytes]
	offset += nullBytes
	values := make([]columnValue, 0, count)
	for i := 0; i < count; i++ {
		byteIndex := i / 8
		bitIndex := uint(i % 8)
		isNull := byteIndex < len(nullBitmap) && (nullBitmap[byteIndex]&(1<<bitIndex)) != 0
		if isNull {
			values = append(values, columnValue{null: true})
			continue
		}
		if offset+4 > len(payload) {
			return nil, errors.New("row data truncated")
		}
		length := int(int32(binary.LittleEndian.Uint32(payload[offset : offset+4])))
		offset += 4
		if length < 0 {
			values = append(values, columnValue{null: true})
			continue
		}
		if offset+length > len(payload) {
			return nil, errors.New("row data truncated")
		}
		data := append([]byte{}, payload[offset:offset+length]...)
		offset += length
		values = append(values, columnValue{data: data})
	}
	return values, nil
}

func parseCommandComplete(payload []byte) (byte, uint64, uint64, string, error) {
	if len(payload) < 4+8+8 {
		return 0, 0, 0, "", errors.New("command complete truncated")
	}
	commandType := payload[0]
	rows := binary.LittleEndian.Uint64(payload[4:12])
	lastID := binary.LittleEndian.Uint64(payload[12:20])
	tagBytes := payload[20:]
	tag := string(tagBytes)
	for i, ch := range tagBytes {
		if ch == 0 {
			tag = string(tagBytes[:i])
			break
		}
	}
	return commandType, rows, lastID, tag, nil
}

type notificationMessage struct {
	processID uint32
	channel   string
	payload   []byte
	change    byte
	rowID     uint64
	hasRow    bool
}

func parseNotification(payload []byte) (notificationMessage, error) {
	if len(payload) < 12 {
		return notificationMessage{}, errors.New("notification truncated")
	}
	offset := 0
	processID := binary.LittleEndian.Uint32(payload[offset : offset+4])
	offset += 4
	channelLen := int(binary.LittleEndian.Uint32(payload[offset : offset+4]))
	offset += 4
	if offset+channelLen+4 > len(payload) {
		return notificationMessage{}, errors.New("notification truncated")
	}
	channel := string(payload[offset : offset+channelLen])
	offset += channelLen
	payloadLen := int(binary.LittleEndian.Uint32(payload[offset : offset+4]))
	offset += 4
	if offset+payloadLen > len(payload) {
		return notificationMessage{}, errors.New("notification truncated")
	}
	data := payload[offset : offset+payloadLen]
	offset += payloadLen
	var change byte
	var rowID uint64
	hasRow := false
	if offset < len(payload) {
		change = payload[offset]
		offset++
		if offset+8 <= len(payload) {
			rowID = binary.LittleEndian.Uint64(payload[offset : offset+8])
			hasRow = true
		}
	}
	return notificationMessage{
		processID: processID,
		channel:   channel,
		payload:   append([]byte(nil), data...),
		change:    change,
		rowID:     rowID,
		hasRow:    hasRow,
	}, nil
}

type queryPlan struct {
	format        uint32
	planningTime  uint64
	estimatedRows uint64
	estimatedCost uint64
	plan          []byte
}

func parseQueryPlan(payload []byte) (queryPlan, error) {
	if len(payload) < 32 {
		return queryPlan{}, errors.New("query plan truncated")
	}
	format := binary.LittleEndian.Uint32(payload[0:4])
	planLength := int(binary.LittleEndian.Uint32(payload[4:8]))
	planning := binary.LittleEndian.Uint64(payload[8:16])
	estimatedRows := binary.LittleEndian.Uint64(payload[16:24])
	estimatedCost := binary.LittleEndian.Uint64(payload[24:32])
	if 32+planLength > len(payload) {
		return queryPlan{}, errors.New("query plan truncated")
	}
	plan := payload[32 : 32+planLength]
	return queryPlan{
		format:        format,
		planningTime:  planning,
		estimatedRows: estimatedRows,
		estimatedCost: estimatedCost,
		plan:          append([]byte(nil), plan...),
	}, nil
}

type sblrCompiled struct {
	hash     uint64
	version  uint32
	bytecode []byte
}

func parseSblrCompiled(payload []byte) (sblrCompiled, error) {
	if len(payload) < 16 {
		return sblrCompiled{}, errors.New("sblr compiled truncated")
	}
	hash := binary.LittleEndian.Uint64(payload[0:8])
	version := binary.LittleEndian.Uint32(payload[8:12])
	length := int(binary.LittleEndian.Uint32(payload[12:16]))
	if 16+length > len(payload) {
		return sblrCompiled{}, errors.New("sblr compiled truncated")
	}
	bytecode := payload[16 : 16+length]
	return sblrCompiled{hash: hash, version: version, bytecode: append([]byte(nil), bytecode...)}, nil
}

func parseErrorMessage(payload []byte) (string, string, string, string, string, error) {
	var severity, sqlState, msg, detail, hint string
	offset := 0
	for offset < len(payload) {
		field := payload[offset]
		offset++
		if field == 0 {
			break
		}
		start := offset
		for offset < len(payload) && payload[offset] != 0 {
			offset++
		}
		if offset >= len(payload) {
			return "", "", "", "", "", errors.New("error message truncated")
		}
		value := string(payload[start:offset])
		offset++
		switch field {
		case 'S':
			severity = value
		case 'C':
			sqlState = value
		case 'M':
			msg = value
		case 'D':
			detail = value
		case 'H':
			hint = value
		}
	}
	return severity, sqlState, msg, detail, hint, nil
}

// ============================================================================
// COPY Message Builders (SBWP 1.1)
// ============================================================================

func buildCopyDataPayload(data []byte) []byte {
	return data
}

func buildCopyDonePayload() []byte {
	return []byte{}
}

func buildCopyFailPayload(errorMessage string) []byte {
	msg := []byte(errorMessage)
	payload := make([]byte, 4+len(msg))
	binary.LittleEndian.PutUint32(payload[0:4], uint32(len(msg)))
	copy(payload[4:], msg)
	return payload
}

// ============================================================================
// COPY Message Parsers (SBWP 1.1)
// ============================================================================

type CopyInResponse struct {
	Format      byte
	WindowBytes uint32
}

type CopyOutResponse struct {
	Format        byte
	ColumnCount   uint16
	ColumnFormats []uint32
}

type CopyBothResponse struct {
	Format      byte
	WindowBytes uint32
}

func parseCopyInResponse(payload []byte) (CopyInResponse, error) {
	if len(payload) < 5 {
		return CopyInResponse{}, errors.New("copy in response truncated")
	}
	return CopyInResponse{
		Format:      payload[0],
		WindowBytes: binary.LittleEndian.Uint32(payload[1:5]),
	}, nil
}

func parseCopyOutResponse(payload []byte) (CopyOutResponse, error) {
	if len(payload) < 3 {
		return CopyOutResponse{}, errors.New("copy out response truncated")
	}
	format := payload[0]
	colCount := binary.LittleEndian.Uint16(payload[1:3])
	offset := 3
	colFormats := make([]uint32, 0, colCount)
	for i := uint16(0); i < colCount; i++ {
		if offset+4 > len(payload) {
			return CopyOutResponse{}, errors.New("copy out response truncated")
		}
		colFormats = append(colFormats, binary.LittleEndian.Uint32(payload[offset:offset+4]))
		offset += 4
	}
	return CopyOutResponse{
		Format:        format,
		ColumnCount:   colCount,
		ColumnFormats: colFormats,
	}, nil
}

func parseCopyBothResponse(payload []byte) (CopyBothResponse, error) {
	response, err := parseCopyInResponse(payload)
	if err != nil {
		return CopyBothResponse{}, err
	}
	return CopyBothResponse{
		Format:      response.Format,
		WindowBytes: response.WindowBytes,
	}, nil
}
