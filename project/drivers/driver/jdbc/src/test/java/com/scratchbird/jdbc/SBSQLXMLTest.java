// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.OutputStream;
import java.io.Reader;
import java.io.StringReader;
import java.io.Writer;
import javax.xml.stream.XMLInputFactory;
import javax.xml.stream.XMLOutputFactory;
import javax.xml.stream.XMLEventReader;
import javax.xml.stream.XMLEventWriter;
import javax.xml.stream.XMLStreamReader;
import javax.xml.stream.XMLStreamWriter;
import javax.xml.transform.Result;
import javax.xml.transform.Source;
import javax.xml.transform.TransformerFactory;
import javax.xml.transform.dom.DOMResult;
import javax.xml.transform.dom.DOMSource;
import javax.xml.transform.sax.SAXResult;
import javax.xml.transform.sax.SAXSource;
import javax.xml.transform.stax.StAXResult;
import javax.xml.transform.stax.StAXSource;
import javax.xml.transform.stream.StreamResult;
import javax.xml.transform.stream.StreamSource;
import org.junit.jupiter.api.Test;
import org.w3c.dom.Node;
import org.xml.sax.ContentHandler;
import org.xml.sax.InputSource;

class SBSQLXMLTest {

    public interface SourceInterfaceAdapter extends Source {
        Source delegate();
    }

    public interface ResultInterfaceAdapter extends Result {
        Result delegate();
    }

    public static class CustomStreamSource extends StreamSource {
    }

    public static class CustomDomSource extends DOMSource {
    }

    public static class CustomStreamResult extends StreamResult {
    }

    public static class CustomDomResult extends DOMResult {
    }

    public static class StringCtorSourceWrapper implements Source {
        private final String payload;
        private String systemId;

        public StringCtorSourceWrapper(String payload) {
            this.payload = payload;
        }

        public String payload() {
            return payload;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class WriterCtorResultWrapper implements Result {
        private final Writer writer;
        private String systemId;

        public WriterCtorResultWrapper(Writer writer) {
            this.writer = writer;
        }

        public Writer writer() {
            return writer;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class CustomReaderCtorStreamSource extends StreamSource {
        public CustomReaderCtorStreamSource(Reader reader) {
            super(reader);
        }
    }

    public static class CustomInputSourceCtorSaxSource extends SAXSource {
        public CustomInputSourceCtorSaxSource(InputSource inputSource) {
            super(inputSource);
        }
    }

    public static class CustomNodeCtorDomSource extends DOMSource {
        public CustomNodeCtorDomSource(Node node) {
            super(node);
        }
    }

    public static class CustomXmlStreamReaderCtorStaxSource extends StAXSource {
        public CustomXmlStreamReaderCtorStaxSource(XMLStreamReader reader) {
            super(reader);
        }
    }

    public static class CustomXmlEventReaderCtorStaxSource extends StAXSource {
        public CustomXmlEventReaderCtorStaxSource(XMLEventReader reader) throws javax.xml.stream.XMLStreamException {
            super(reader);
        }
    }

    public static class CustomWriterCtorStreamResult extends StreamResult {
        public CustomWriterCtorStreamResult(Writer writer) {
            super(writer);
        }
    }

    public static class CustomOutputStreamCtorStreamResult extends StreamResult {
        public CustomOutputStreamCtorStreamResult(OutputStream outputStream) {
            super(outputStream);
        }
    }

    public static class CustomNodeCtorDomResult extends DOMResult {
        public CustomNodeCtorDomResult(Node node) {
            super(node);
        }
    }

    public static class CustomContentHandlerCtorSaxResult extends SAXResult {
        public CustomContentHandlerCtorSaxResult(ContentHandler handler) {
            super(handler);
        }
    }

    public static class CustomXmlStreamWriterCtorStaxResult extends StAXResult {
        public CustomXmlStreamWriterCtorStaxResult(XMLStreamWriter writer) {
            super(writer);
        }
    }

    public static class CustomXmlEventWriterCtorStaxResult extends StAXResult {
        public CustomXmlEventWriterCtorStaxResult(XMLEventWriter writer) {
            super(writer);
        }
    }

    public static class CustomDelegatingStaxSource extends StAXSource {
        private final Source delegate;

        public CustomDelegatingStaxSource(Source delegate) {
            super(extractStreamReader(delegate));
            this.delegate = delegate;
        }

        public Source delegate() {
            return delegate;
        }

        private static XMLStreamReader extractStreamReader(Source delegate) {
            if (delegate instanceof StAXSource staxSource && staxSource.getXMLStreamReader() != null) {
                return staxSource.getXMLStreamReader();
            }
            if (delegate instanceof StreamSource streamSource) {
                try {
                    XMLInputFactory inputFactory = XMLInputFactory.newFactory();
                    if (streamSource.getReader() != null) {
                        return inputFactory.createXMLStreamReader(streamSource.getReader());
                    }
                    if (streamSource.getInputStream() != null) {
                        return inputFactory.createXMLStreamReader(streamSource.getInputStream());
                    }
                } catch (Exception ex) {
                    throw new IllegalArgumentException("Failed to create XMLStreamReader from StreamSource", ex);
                }
            }
            throw new IllegalArgumentException("StAXSource delegate with XMLStreamReader expected");
        }
    }

    public static class CustomDelegatingStaxResult extends StAXResult {
        private final Result delegate;

        public CustomDelegatingStaxResult(Result delegate) {
            super(extractStreamWriter(delegate));
            this.delegate = delegate;
        }

        public Result delegate() {
            return delegate;
        }

        private static XMLStreamWriter extractStreamWriter(Result delegate) {
            if (delegate instanceof StAXResult staxResult && staxResult.getXMLStreamWriter() != null) {
                return staxResult.getXMLStreamWriter();
            }
            if (delegate instanceof StreamResult streamResult) {
                try {
                    XMLOutputFactory outputFactory = XMLOutputFactory.newFactory();
                    if (streamResult.getWriter() != null) {
                        return outputFactory.createXMLStreamWriter(streamResult.getWriter());
                    }
                    if (streamResult.getOutputStream() != null) {
                        return outputFactory.createXMLStreamWriter(streamResult.getOutputStream(), "UTF-8");
                    }
                } catch (Exception ex) {
                    throw new IllegalArgumentException("Failed to create XMLStreamWriter from StreamResult", ex);
                }
            }
            throw new IllegalArgumentException("StAXResult delegate with XMLStreamWriter expected");
        }
    }

    public static class StaticFactoryDelegatingSource implements Source {
        private final Source delegate;
        private String systemId;

        private StaticFactoryDelegatingSource(Source delegate) {
            this.delegate = delegate;
            this.systemId = delegate == null ? null : delegate.getSystemId();
        }

        public static StaticFactoryDelegatingSource of(Source delegate) {
            return new StaticFactoryDelegatingSource(delegate);
        }

        public Source delegate() {
            return delegate;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class StaticFactoryDelegatingResult implements Result {
        private final Result delegate;
        private String systemId;

        private StaticFactoryDelegatingResult(Result delegate) {
            this.delegate = delegate;
            this.systemId = delegate == null ? null : delegate.getSystemId();
        }

        public static StaticFactoryDelegatingResult from(Result delegate) {
            return new StaticFactoryDelegatingResult(delegate);
        }

        public Result delegate() {
            return delegate;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class ObjectFactoryDelegatingSource implements Source {
        private final Object delegate;
        private String systemId;

        private ObjectFactoryDelegatingSource(Object delegate) {
            this.delegate = delegate;
            this.systemId = delegate instanceof Source source ? source.getSystemId() : null;
        }

        public static ObjectFactoryDelegatingSource of(Object delegate) {
            return new ObjectFactoryDelegatingSource(delegate);
        }

        public Object delegate() {
            return delegate;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class ObjectFactoryDelegatingResult implements Result {
        private final Object delegate;
        private String systemId;

        private ObjectFactoryDelegatingResult(Object delegate) {
            this.delegate = delegate;
            this.systemId = delegate instanceof Result result ? result.getSystemId() : null;
        }

        public static ObjectFactoryDelegatingResult from(Object delegate) {
            return new ObjectFactoryDelegatingResult(delegate);
        }

        public Object delegate() {
            return delegate;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class CreateFactoryDelegatingSource implements Source {
        private final Source delegate;
        private String systemId;

        private CreateFactoryDelegatingSource(Source delegate) {
            this.delegate = delegate;
            this.systemId = delegate == null ? null : delegate.getSystemId();
        }

        public static CreateFactoryDelegatingSource create(Source delegate) {
            return new CreateFactoryDelegatingSource(delegate);
        }

        public Source delegate() {
            return delegate;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class CreateFactoryDelegatingResult implements Result {
        private final Result delegate;
        private String systemId;

        private CreateFactoryDelegatingResult(Result delegate) {
            this.delegate = delegate;
            this.systemId = delegate == null ? null : delegate.getSystemId();
        }

        public static CreateFactoryDelegatingResult create(Result delegate) {
            return new CreateFactoryDelegatingResult(delegate);
        }

        public Result delegate() {
            return delegate;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class CustomDelegatingSource implements Source {
        private final Source delegate;
        private String systemId;

        public CustomDelegatingSource(Source delegate) {
            this.delegate = delegate;
            this.systemId = delegate == null ? null : delegate.getSystemId();
        }

        public Source delegate() {
            return delegate;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class CustomDelegatingResult implements Result {
        private final Result delegate;
        private String systemId;

        public CustomDelegatingResult(Result delegate) {
            this.delegate = delegate;
            this.systemId = delegate == null ? null : delegate.getSystemId();
        }

        public Result delegate() {
            return delegate;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class SetterDelegatingSource implements Source {
        private Source delegate;
        private String systemId;

        public SetterDelegatingSource() {
        }

        public void setDelegate(Source delegate) {
            this.delegate = delegate;
            if (delegate != null) {
                this.systemId = delegate.getSystemId();
            }
        }

        public Source delegate() {
            return delegate;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class SetterDelegatingResult implements Result {
        private Result delegate;
        private String systemId;

        public SetterDelegatingResult() {
        }

        public void setDelegate(Result delegate) {
            this.delegate = delegate;
            if (delegate != null) {
                this.systemId = delegate.getSystemId();
            }
        }

        public Result delegate() {
            return delegate;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class NonSetterDelegatingSource implements Source {
        private Source wrapped;
        private String systemId;

        public void bindWrappedSource(Source delegate) {
            this.wrapped = delegate;
            if (delegate != null) {
                this.systemId = delegate.getSystemId();
            }
        }

        public Source delegate() {
            return wrapped;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class NonSetterDelegatingResult implements Result {
        private Result wrapped;
        private String systemId;

        public void bindWrappedResult(Result delegate) {
            this.wrapped = delegate;
            if (delegate != null) {
                this.systemId = delegate.getSystemId();
            }
        }

        public Result delegate() {
            return wrapped;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class FieldOnlySourceWrapper implements Source {
        private Source delegate;
        private String systemId;

        public Source delegate() {
            return delegate;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class FieldOnlyResultWrapper implements Result {
        private Result delegate;
        private String systemId;

        public Result delegate() {
            return delegate;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class UnsafeOnlySourceWrapper implements Source {
        private Source delegate;
        private String systemId;

        public UnsafeOnlySourceWrapper(Object marker) {
            throw new IllegalStateException("Constructor path should not be used for unsafe fallback.");
        }

        public Source delegate() {
            return delegate;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    public static class UnsafeOnlyResultWrapper implements Result {
        private Result delegate;
        private String systemId;

        public UnsafeOnlyResultWrapper(Object marker) {
            throw new IllegalStateException("Constructor path should not be used for unsafe fallback.");
        }

        public Result delegate() {
            return delegate;
        }

        @Override
        public void setSystemId(String systemId) {
            this.systemId = systemId;
        }

        @Override
        public String getSystemId() {
            return systemId;
        }
    }

    @Test
    void supportsDomSourceAndDomResultRoundTrip() throws Exception {
        SBSQLXML sourceXml = new SBSQLXML("<root><value>1</value></root>");
        DOMSource source = sourceXml.getSource(DOMSource.class);
        assertNotNull(source.getNode());

        SBSQLXML target = new SBSQLXML();
        DOMResult result = target.setResult(DOMResult.class);
        TransformerFactory.newInstance().newTransformer().transform(source, result);

        String xml = target.getString();
        assertNotNull(xml);
        assertTrue(xml.contains("root"));
        assertTrue(xml.contains("value"));
    }

    @Test
    void supportsSaxAndStaxSourcesAndResults() throws Exception {
        SBSQLXML sourceXml = new SBSQLXML("<root><n>7</n></root>");
        SAXSource saxSource = sourceXml.getSource(SAXSource.class);
        assertNotNull(saxSource.getInputSource());
        StAXSource staxSource = sourceXml.getSource(StAXSource.class);
        assertNotNull(staxSource.getXMLStreamReader());

        SBSQLXML saxTarget = new SBSQLXML();
        SAXResult saxResult = saxTarget.setResult(SAXResult.class);
        TransformerFactory.newInstance().newTransformer().transform(
            new StreamSource(new StringReader("<sax><ok>true</ok></sax>")), saxResult);
        assertTrue(saxTarget.getString().contains("sax"));

        SBSQLXML staxTarget = new SBSQLXML();
        StAXResult staxResult = staxTarget.setResult(StAXResult.class);
        XMLStreamWriter writer = staxResult.getXMLStreamWriter();
        writer.writeStartDocument();
        writer.writeStartElement("stax");
        writer.writeCharacters("ok");
        writer.writeEndElement();
        writer.writeEndDocument();
        assertTrue(staxTarget.getString().contains("<stax>ok</stax>"));
    }

    @Test
    void nullResultClassUsesStreamResultAndMaterializesOnRead() throws Exception {
        SBSQLXML sqlxml = new SBSQLXML();
        StreamResult result = sqlxml.setResult(null);
        Writer writer = result.getWriter();
        writer.write("<doc/>");
        writer.close();

        assertEquals("<doc/>", sqlxml.getString());
    }

    @Test
    void genericSourceAndResultClassRequestsUseStreamImplementations() throws Exception {
        SBSQLXML sqlxml = new SBSQLXML("<root/>");
        Source source = sqlxml.getSource(Source.class);
        assertTrue(source instanceof StreamSource);

        SBSQLXML target = new SBSQLXML();
        Result result = target.setResult(Result.class);
        assertTrue(result instanceof StreamResult);
        Writer writer = ((StreamResult) result).getWriter();
        writer.write("<generic/>");
        writer.close();
        assertEquals("<generic/>", target.getString());
    }

    @Test
    void supportsSubclassedSourceAndResultTypes() throws Exception {
        SBSQLXML sourceXml = new SBSQLXML("<root><value>9</value></root>");
        CustomStreamSource customStreamSource = sourceXml.getSource(CustomStreamSource.class);
        assertNotNull(customStreamSource.getReader());

        CustomDomSource customDomSource = sourceXml.getSource(CustomDomSource.class);
        assertNotNull(customDomSource.getNode());

        SBSQLXML streamTarget = new SBSQLXML();
        CustomStreamResult customStreamResult = streamTarget.setResult(CustomStreamResult.class);
        Writer streamWriter = customStreamResult.getWriter();
        streamWriter.write("<s/>");
        streamWriter.close();
        assertEquals("<s/>", streamTarget.getString());

        SBSQLXML domTarget = new SBSQLXML();
        CustomDomResult customDomResult = domTarget.setResult(CustomDomResult.class);
        TransformerFactory.newInstance().newTransformer().transform(
            new StreamSource(new StringReader("<d/>")), customDomResult);
        assertTrue(domTarget.getString().contains("d"));
    }

    @Test
    void supportsStringAndWriterCtorWrappersForGenericSourceAndResultClasses() throws Exception {
        SBSQLXML sourceXml = new SBSQLXML("<ctor-source/>");
        StringCtorSourceWrapper sourceWrapper = sourceXml.getSource(StringCtorSourceWrapper.class);
        assertNotNull(sourceWrapper);
        assertEquals("<ctor-source/>", sourceWrapper.payload());

        SBSQLXML target = new SBSQLXML();
        WriterCtorResultWrapper resultWrapper = target.setResult(WriterCtorResultWrapper.class);
        assertNotNull(resultWrapper);
        Writer writer = resultWrapper.writer();
        assertNotNull(writer);
        writer.write("<ctor-result/>");
        writer.close();
        assertEquals("<ctor-result/>", target.getString());
    }

    @Test
    void supportsSubclassConstructorsWithoutDefaultCtorAcrossSourceAndResultFamilies() throws Exception {
        SBSQLXML sourceXml = new SBSQLXML("<root><value>11</value></root>");
        CustomReaderCtorStreamSource streamSource = sourceXml.getSource(CustomReaderCtorStreamSource.class);
        assertNotNull(streamSource.getReader());
        CustomInputSourceCtorSaxSource saxSource = sourceXml.getSource(CustomInputSourceCtorSaxSource.class);
        assertNotNull(saxSource.getInputSource());
        CustomNodeCtorDomSource domSource = sourceXml.getSource(CustomNodeCtorDomSource.class);
        assertNotNull(domSource.getNode());
        CustomXmlStreamReaderCtorStaxSource staxSource =
            sourceXml.getSource(CustomXmlStreamReaderCtorStaxSource.class);
        assertNotNull(staxSource.getXMLStreamReader());
        CustomXmlEventReaderCtorStaxSource staxEventSource =
            sourceXml.getSource(CustomXmlEventReaderCtorStaxSource.class);
        assertNotNull(staxEventSource.getXMLEventReader());

        SBSQLXML streamWriterTarget = new SBSQLXML();
        CustomWriterCtorStreamResult writerResult =
            streamWriterTarget.setResult(CustomWriterCtorStreamResult.class);
        assertNotNull(writerResult.getWriter());
        writerResult.getWriter().write("<writer/>");
        writerResult.getWriter().close();
        assertTrue(streamWriterTarget.getString().contains("writer"));

        SBSQLXML streamBinaryTarget = new SBSQLXML();
        CustomOutputStreamCtorStreamResult outputResult =
            streamBinaryTarget.setResult(CustomOutputStreamCtorStreamResult.class);
        assertNotNull(outputResult.getOutputStream());
        outputResult.getOutputStream().write("<binary/>".getBytes());
        outputResult.getOutputStream().close();
        assertTrue(streamBinaryTarget.getString().contains("binary"));

        SBSQLXML domTarget = new SBSQLXML();
        CustomNodeCtorDomResult customDomResult = domTarget.setResult(CustomNodeCtorDomResult.class);
        TransformerFactory.newInstance().newTransformer().transform(
            new StreamSource(new StringReader("<dom/>")), customDomResult);
        assertTrue(domTarget.getString().contains("dom"));

        SBSQLXML saxTarget = new SBSQLXML();
        CustomContentHandlerCtorSaxResult customSaxResult =
            saxTarget.setResult(CustomContentHandlerCtorSaxResult.class);
        TransformerFactory.newInstance().newTransformer().transform(
            new StreamSource(new StringReader("<saxctor/>")), customSaxResult);
        assertTrue(saxTarget.getString().contains("saxctor"));

        SBSQLXML staxTarget = new SBSQLXML();
        CustomXmlStreamWriterCtorStaxResult customStaxResult =
            staxTarget.setResult(CustomXmlStreamWriterCtorStaxResult.class);
        XMLStreamWriter writer = customStaxResult.getXMLStreamWriter();
        writer.writeStartDocument();
        writer.writeStartElement("staxctor");
        writer.writeEndElement();
        writer.writeEndDocument();
        assertTrue(staxTarget.getString().contains("staxctor"));

        SBSQLXML staxEventTarget = new SBSQLXML();
        CustomXmlEventWriterCtorStaxResult customStaxEventResult =
            staxEventTarget.setResult(CustomXmlEventWriterCtorStaxResult.class);
        TransformerFactory.newInstance().newTransformer().transform(
            new StreamSource(new StringReader("<staxevent/>")), customStaxEventResult);
        String staxEventXml = staxEventTarget.getString();
        assertNotNull(staxEventXml);
        assertTrue(staxEventXml.contains("staxevent"));
    }

    @Test
    void supportsDelegatingConstructorsForGenericSourceAndResultWrappers() throws Exception {
        SBSQLXML sourceXml = new SBSQLXML("<root><value>13</value></root>");
        CustomDelegatingSource source = sourceXml.getSource(CustomDelegatingSource.class);
        assertNotNull(source);
        assertTrue(source.delegate() instanceof StreamSource);
        assertNotNull(((StreamSource) source.delegate()).getReader());

        SBSQLXML target = new SBSQLXML();
        CustomDelegatingResult result = target.setResult(CustomDelegatingResult.class);
        assertNotNull(result);
        assertTrue(result.delegate() instanceof StreamResult);
        Writer writer = ((StreamResult) result.delegate()).getWriter();
        assertNotNull(writer);
        writer.write("<delegated/>");
        writer.close();
        assertEquals("<delegated/>", target.getString());
    }

    @Test
    void supportsSetterDelegatingSourceAndResultWrappersWithoutDelegateConstructors() throws Exception {
        SBSQLXML sourceXml = new SBSQLXML("<root><value>17</value></root>");
        SetterDelegatingSource source = sourceXml.getSource(SetterDelegatingSource.class);
        assertNotNull(source);
        assertTrue(source.delegate() instanceof StreamSource);
        assertNotNull(((StreamSource) source.delegate()).getReader());

        SBSQLXML target = new SBSQLXML();
        SetterDelegatingResult result = target.setResult(SetterDelegatingResult.class);
        assertNotNull(result);
        assertTrue(result.delegate() instanceof StreamResult);
        Writer writer = ((StreamResult) result.delegate()).getWriter();
        assertNotNull(writer);
        writer.write("<setter-delegated/>");
        writer.close();
        assertEquals("<setter-delegated/>", target.getString());
    }

    @Test
    void supportsNonSetterDelegateMutationHooksForSourceAndResultWrappers() throws Exception {
        SBSQLXML sourceXml = new SBSQLXML("<root><value>19</value></root>");
        NonSetterDelegatingSource source = sourceXml.getSource(NonSetterDelegatingSource.class);
        assertNotNull(source);
        assertNotNull(source.delegate());
        assertTrue(source.delegate() instanceof StreamSource || source.delegate() instanceof Source);

        SBSQLXML target = new SBSQLXML();
        NonSetterDelegatingResult result = target.setResult(NonSetterDelegatingResult.class);
        assertNotNull(result);
        assertNotNull(result.delegate());
        assertTrue(result.delegate() instanceof StreamResult || result.delegate() instanceof Result);

        Writer writer = ((StreamResult) result.delegate()).getWriter();
        assertNotNull(writer);
        writer.write("<non-setter/>");
        writer.close();
        assertEquals("<non-setter/>", target.getString());
    }

    @Test
    void supportsStaxSubclassConstructorsViaDelegateFallback() throws Exception {
        SBSQLXML sourceXml = new SBSQLXML("<root><value>23</value></root>");
        CustomDelegatingStaxSource source = sourceXml.getSource(CustomDelegatingStaxSource.class);
        assertNotNull(source);
        assertNotNull(source.delegate());
        assertTrue(source.delegate() instanceof Source);

        SBSQLXML target = new SBSQLXML();
        CustomDelegatingStaxResult result = target.setResult(CustomDelegatingStaxResult.class);
        assertNotNull(result);
        assertNotNull(result.delegate());
        assertTrue(result.delegate() instanceof Result);
        TransformerFactory.newInstance().newTransformer().transform(
            new StreamSource(new StringReader("<stax-delegate/>")), result);
        String xml = target.getString();
        assertNotNull(xml);
        assertTrue(xml.contains("stax-delegate"));
    }

    @Test
    void supportsStaticFactoryDelegatingSourceAndResultWrappers() throws Exception {
        SBSQLXML sourceXml = new SBSQLXML("<root><value>29</value></root>");
        StaticFactoryDelegatingSource source = sourceXml.getSource(StaticFactoryDelegatingSource.class);
        assertNotNull(source);
        assertNotNull(source.delegate());
        assertTrue(source.delegate() instanceof StreamSource || source.delegate() instanceof Source);

        SBSQLXML target = new SBSQLXML();
        StaticFactoryDelegatingResult result = target.setResult(StaticFactoryDelegatingResult.class);
        assertNotNull(result);
        assertNotNull(result.delegate());
        assertTrue(result.delegate() instanceof StreamResult || result.delegate() instanceof Result);

        Writer writer = ((StreamResult) result.delegate()).getWriter();
        assertNotNull(writer);
        writer.write("<factory-delegated/>");
        writer.close();
        assertEquals("<factory-delegated/>", target.getString());
    }

    @Test
    void supportsAssignableStaticFactoryDelegatesUsingObjectSignatures() throws Exception {
        SBSQLXML sourceXml = new SBSQLXML("<root><value>31</value></root>");
        ObjectFactoryDelegatingSource source = sourceXml.getSource(ObjectFactoryDelegatingSource.class);
        assertNotNull(source);
        assertNotNull(source.delegate());
        assertTrue(source.delegate() instanceof Source);

        SBSQLXML target = new SBSQLXML();
        ObjectFactoryDelegatingResult result = target.setResult(ObjectFactoryDelegatingResult.class);
        assertNotNull(result);
        assertNotNull(result.delegate());
        assertTrue(result.delegate() instanceof Result);

        Writer writer = ((StreamResult) result.delegate()).getWriter();
        assertNotNull(writer);
        writer.write("<object-factory/>");
        writer.close();
        assertEquals("<object-factory/>", target.getString());
    }

    @Test
    void supportsCreateFactoryDelegatesForSourceAndResultWrappers() throws Exception {
        SBSQLXML sourceXml = new SBSQLXML("<root><value>37</value></root>");
        CreateFactoryDelegatingSource source = sourceXml.getSource(CreateFactoryDelegatingSource.class);
        assertNotNull(source);
        assertNotNull(source.delegate());
        assertTrue(source.delegate() instanceof Source);

        SBSQLXML target = new SBSQLXML();
        CreateFactoryDelegatingResult result = target.setResult(CreateFactoryDelegatingResult.class);
        assertNotNull(result);
        assertNotNull(result.delegate());
        assertTrue(result.delegate() instanceof Result);

        Writer writer = ((StreamResult) result.delegate()).getWriter();
        assertNotNull(writer);
        writer.write("<create-factory/>");
        writer.close();
        assertEquals("<create-factory/>", target.getString());
    }

    @Test
    void supportsFieldOnlyDelegatingSourceAndResultWrappers() throws Exception {
        SBSQLXML sourceXml = new SBSQLXML("<root><value>41</value></root>");
        FieldOnlySourceWrapper source = sourceXml.getSource(FieldOnlySourceWrapper.class);
        assertNotNull(source);
        assertNotNull(source.delegate());
        assertTrue(source.delegate() instanceof Source);

        SBSQLXML target = new SBSQLXML();
        FieldOnlyResultWrapper result = target.setResult(FieldOnlyResultWrapper.class);
        assertNotNull(result);
        assertNotNull(result.delegate());
        assertTrue(result.delegate() instanceof StreamResult || result.delegate() instanceof Result);

        Writer writer = ((StreamResult) result.delegate()).getWriter();
        assertNotNull(writer);
        writer.write("<field-only/>");
        writer.close();
        assertEquals("<field-only/>", target.getString());
    }

    @Test
    void supportsUnsafeConstructionFallbackForConstructorHostileWrappers() throws Exception {
        SBSQLXML sourceXml = new SBSQLXML("<root><value>43</value></root>");
        UnsafeOnlySourceWrapper source = sourceXml.getSource(UnsafeOnlySourceWrapper.class);
        assertNotNull(source);
        assertNotNull(source.delegate());
        assertTrue(source.delegate() instanceof Source);

        SBSQLXML target = new SBSQLXML();
        UnsafeOnlyResultWrapper result = target.setResult(UnsafeOnlyResultWrapper.class);
        assertNotNull(result);
        assertNotNull(result.delegate());
        assertTrue(result.delegate() instanceof StreamResult || result.delegate() instanceof Result);

        Writer writer = ((StreamResult) result.delegate()).getWriter();
        assertNotNull(writer);
        writer.write("<unsafe-only/>");
        writer.close();
        assertEquals("<unsafe-only/>", target.getString());
    }

    @Test
    void supportsInterfaceAdaptersForCustomSourceAndResultInterfaces() throws Exception {
        SBSQLXML sourceXml = new SBSQLXML("<root><value>47</value></root>");
        SourceInterfaceAdapter source = sourceXml.getSource(SourceInterfaceAdapter.class);
        assertNotNull(source);
        assertNotNull(source.delegate());
        assertTrue(source.delegate() instanceof Source);
        assertEquals(sourceXml.getSource(StreamSource.class).getSystemId(), source.getSystemId());

        SBSQLXML target = new SBSQLXML();
        ResultInterfaceAdapter result = target.setResult(ResultInterfaceAdapter.class);
        assertNotNull(result);
        assertNotNull(result.delegate());
        assertTrue(result.delegate() instanceof StreamResult || result.delegate() instanceof Result);

        Writer writer = ((StreamResult) result.delegate()).getWriter();
        writer.write("<interface-adapter/>");
        writer.close();
        assertEquals("<interface-adapter/>", target.getString());
    }
}
