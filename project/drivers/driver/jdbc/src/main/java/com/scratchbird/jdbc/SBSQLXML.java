// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver
 * Copyright (c) 2025 ScratchBird Project
 */
package com.scratchbird.jdbc;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.Reader;
import java.io.StringReader;
import java.io.StringWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.sql.SQLException;
import java.sql.SQLFeatureNotSupportedException;
import java.sql.SQLXML;
import javax.xml.XMLConstants;
import javax.xml.parsers.DocumentBuilder;
import javax.xml.parsers.DocumentBuilderFactory;
import javax.xml.stream.XMLInputFactory;
import javax.xml.stream.XMLOutputFactory;
import javax.xml.stream.XMLEventReader;
import javax.xml.stream.XMLEventWriter;
import javax.xml.stream.XMLStreamReader;
import javax.xml.stream.XMLStreamWriter;
import javax.xml.transform.Result;
import javax.xml.transform.Source;
import javax.xml.transform.Transformer;
import javax.xml.transform.TransformerFactory;
import javax.xml.transform.dom.DOMResult;
import javax.xml.transform.dom.DOMSource;
import javax.xml.transform.sax.SAXResult;
import javax.xml.transform.sax.SAXSource;
import javax.xml.transform.sax.SAXTransformerFactory;
import javax.xml.transform.sax.TransformerHandler;
import javax.xml.transform.stax.StAXResult;
import javax.xml.transform.stax.StAXSource;
import javax.xml.transform.stream.StreamResult;
import javax.xml.transform.stream.StreamSource;
import org.w3c.dom.Node;
import org.xml.sax.Attributes;
import org.xml.sax.InputSource;
import org.xml.sax.XMLReader;
import org.xml.sax.helpers.DefaultHandler;

/**
 * JDBC SQLXML implementation for ScratchBird.
 */
public class SBSQLXML implements SQLXML {
    @FunctionalInterface
    private interface Materializer {
        String materialize() throws Exception;
    }

    private String data;
    private boolean freed = false;
    private Materializer pendingMaterializer;

    public SBSQLXML() {
        this.data = null;
    }

    public SBSQLXML(String xml) {
        this.data = xml;
    }

    @Override
    public void free() throws SQLException {
        data = null;
        freed = true;
        pendingMaterializer = null;
    }

    @Override
    public InputStream getBinaryStream() throws SQLException {
        checkFreed();
        materializeIfPending();
        if (data == null) return null;
        return new ByteArrayInputStream(data.getBytes(StandardCharsets.UTF_8));
    }

    @Override
    public OutputStream setBinaryStream() throws SQLException {
        checkFreed();
        pendingMaterializer = null;
        return new ByteArrayOutputStream() {
            @Override
            public void close() {
                data = new String(toByteArray(), StandardCharsets.UTF_8);
            }
        };
    }

    @Override
    public Reader getCharacterStream() throws SQLException {
        checkFreed();
        materializeIfPending();
        if (data == null) return null;
        return new StringReader(data);
    }

    @Override
    public Writer setCharacterStream() throws SQLException {
        checkFreed();
        pendingMaterializer = null;
        return new StringWriter() {
            @Override
            public void close() {
                data = toString();
            }
        };
    }

    @Override
    public String getString() throws SQLException {
        checkFreed();
        materializeIfPending();
        return data;
    }

    @Override
    public void setString(String value) throws SQLException {
        checkFreed();
        this.data = value;
        this.pendingMaterializer = null;
    }

    @Override
    public <T extends Source> T getSource(Class<T> sourceClass) throws SQLException {
        checkFreed();
        materializeIfPending();

        String xml = data == null ? "" : data;
        if (sourceClass == null || sourceClass == Source.class || sourceClass == StreamSource.class) {
            @SuppressWarnings("unchecked")
            T streamSource = (T) new StreamSource(new StringReader(xml));
            return streamSource;
        }
        if (sourceClass == DOMSource.class) {
            @SuppressWarnings("unchecked")
            T domSource = (T) new DOMSource(parseDom(xml));
            return domSource;
        }
        if (sourceClass == SAXSource.class) {
            @SuppressWarnings("unchecked")
            T saxSource = (T) new SAXSource(new InputSource(new StringReader(xml)));
            return saxSource;
        }
        if (sourceClass == StAXSource.class) {
            try {
                XMLInputFactory factory = XMLInputFactory.newFactory();
                XMLStreamReader reader = factory.createXMLStreamReader(new StringReader(xml));
                @SuppressWarnings("unchecked")
                T staxSource = (T) new StAXSource(reader);
                return staxSource;
            } catch (Exception ex) {
                throw new SQLException("Failed to build StAXSource from SQLXML", "HY000", ex);
            }
        }
        if (sourceClass != null && StreamSource.class.isAssignableFrom(sourceClass)) {
            try {
                return instantiateStreamSourceSubclass(sourceClass, xml);
            } catch (SQLFeatureNotSupportedException ignored) {
                // Fall through to delegate-based instantiation paths.
            }
        }
        if (sourceClass != null && DOMSource.class.isAssignableFrom(sourceClass)) {
            try {
                return instantiateDomSourceSubclass(sourceClass, xml);
            } catch (SQLFeatureNotSupportedException ignored) {
                // Fall through to delegate-based instantiation paths.
            }
        }
        if (sourceClass != null && SAXSource.class.isAssignableFrom(sourceClass)) {
            try {
                return instantiateSaxSourceSubclass(sourceClass, xml);
            } catch (SQLFeatureNotSupportedException ignored) {
                // Fall through to delegate-based instantiation paths.
            }
        }
        if (sourceClass != null && StAXSource.class.isAssignableFrom(sourceClass)) {
            try {
                return instantiateStaxSourceSubclass(sourceClass, xml);
            } catch (SQLFeatureNotSupportedException ignored) {
                // Fall through to delegate-based instantiation paths.
            }
        }
        T delegated = instantiateSourceFromDelegate(sourceClass, xml);
        if (delegated != null) {
            return delegated;
        }
        if (sourceClass != null && sourceClass.isInterface() && Source.class.isAssignableFrom(sourceClass)) {
            T adapted = adaptSourceInterface(sourceClass, new StreamSource(new StringReader(xml)));
            if (adapted != null) {
                return adapted;
            }
        }
        T bestEffort = instantiateSourceBestEffort(sourceClass, xml);
        if (bestEffort != null) {
            return bestEffort;
        }
        throw new SQLFeatureNotSupportedException("Source class not supported: " + sourceClass);
    }

    @Override
    public <T extends Result> T setResult(Class<T> resultClass) throws SQLException {
        checkFreed();
        if (resultClass == null || resultClass == Result.class || resultClass == StreamResult.class) {
            StringWriter writer = new StringWriter();
            pendingMaterializer = writer::toString;
            @SuppressWarnings("unchecked")
            T streamResult = (T) new StreamResult(writer);
            return streamResult;
        }
        if (resultClass == DOMResult.class) {
            DOMResult domResult = new DOMResult();
            pendingMaterializer = () -> serializeDom(domResult.getNode());
            @SuppressWarnings("unchecked")
            T result = (T) domResult;
            return result;
        }
        if (resultClass == SAXResult.class) {
            try {
                TransformerFactory transformerFactory = TransformerFactory.newInstance();
                if (transformerFactory instanceof SAXTransformerFactory saxFactory) {
                    TransformerHandler handler = saxFactory.newTransformerHandler();
                    StringWriter writer = new StringWriter();
                    handler.setResult(new StreamResult(writer));
                    pendingMaterializer = writer::toString;
                    @SuppressWarnings("unchecked")
                    T result = (T) new SAXResult(handler);
                    return result;
                }
                SaxCaptureHandler handler = new SaxCaptureHandler();
                pendingMaterializer = handler::toXml;
                @SuppressWarnings("unchecked")
                T result = (T) new SAXResult(handler);
                return result;
            } catch (Exception ex) {
                throw new SQLException("Failed to initialize SAXResult for SQLXML", "HY000", ex);
            }
        }
        if (resultClass == StAXResult.class) {
            try {
                StringWriter writer = new StringWriter();
                XMLOutputFactory outputFactory = XMLOutputFactory.newFactory();
                XMLStreamWriter streamWriter = outputFactory.createXMLStreamWriter(writer);
                pendingMaterializer = () -> {
                    streamWriter.flush();
                    streamWriter.close();
                    return writer.toString();
                };
                @SuppressWarnings("unchecked")
                T result = (T) new StAXResult(streamWriter);
                return result;
            } catch (Exception ex) {
                throw new SQLException("Failed to initialize StAXResult for SQLXML", "HY000", ex);
            }
        }
        if (resultClass != null && StreamResult.class.isAssignableFrom(resultClass)) {
            try {
                return instantiateStreamResultSubclass(resultClass);
            } catch (SQLFeatureNotSupportedException ignored) {
                // Fall through to delegate-based instantiation paths.
            }
        }
        if (resultClass != null && DOMResult.class.isAssignableFrom(resultClass)) {
            try {
                return instantiateDomResultSubclass(resultClass);
            } catch (SQLFeatureNotSupportedException ignored) {
                // Fall through to delegate-based instantiation paths.
            }
        }
        if (resultClass != null && SAXResult.class.isAssignableFrom(resultClass)) {
            try {
                return instantiateSaxResultSubclass(resultClass);
            } catch (SQLFeatureNotSupportedException ignored) {
                // Fall through to delegate-based instantiation paths.
            }
        }
        if (resultClass != null && StAXResult.class.isAssignableFrom(resultClass)) {
            try {
                return instantiateStaxResultSubclass(resultClass);
            } catch (SQLFeatureNotSupportedException ignored) {
                // Fall through to delegate-based instantiation paths.
            }
        }
        T delegated = instantiateResultFromDelegate(resultClass);
        if (delegated != null) {
            return delegated;
        }
        if (resultClass != null && resultClass.isInterface() && Result.class.isAssignableFrom(resultClass)) {
            CandidateResult streamCandidate = streamDelegateResult();
            T adapted = adaptResultInterface(resultClass, streamCandidate.result());
            if (adapted != null) {
                pendingMaterializer = streamCandidate.materializer();
                return adapted;
            }
        }
        T bestEffort = instantiateResultBestEffort(resultClass);
        if (bestEffort != null) {
            return bestEffort;
        }
        throw new SQLFeatureNotSupportedException("Result class not supported: " + resultClass);
    }

    private <T extends Source> T instantiateSourceBestEffort(Class<T> sourceClass, String xml) throws SQLException {
        if (sourceClass == null) {
            return null;
        }
        T instance = instantiateWithDefaultArguments(sourceClass);
        if (instance == null) {
            return null;
        }
        Source delegate = new StreamSource(new StringReader(xml == null ? "" : xml));
        boolean bound = bindDelegateSource(instance, delegate);
        if (!bound) {
            if (instance instanceof StreamSource streamSource && streamSource.getReader() == null) {
                streamSource.setReader(new StringReader(xml == null ? "" : xml));
                bound = true;
            } else if (instance instanceof DOMSource domSource && domSource.getNode() == null) {
                domSource.setNode(parseDom(xml == null ? "" : xml));
                bound = true;
            } else if (instance instanceof SAXSource saxSource && saxSource.getInputSource() == null) {
                saxSource.setInputSource(new InputSource(new StringReader(xml == null ? "" : xml)));
                bound = true;
            }
        }
        if (!bound && instance.getSystemId() == null) {
            instance.setSystemId(delegate.getSystemId());
        }
        return instance;
    }

    private <T extends Result> T instantiateResultBestEffort(Class<T> resultClass) throws SQLException {
        if (resultClass == null) {
            return null;
        }
        T instance = instantiateWithDefaultArguments(resultClass);
        if (instance == null) {
            return null;
        }
        CandidateResult candidate;
        if (DOMResult.class.isAssignableFrom(resultClass)) {
            candidate = domDelegateResult();
        } else if (SAXResult.class.isAssignableFrom(resultClass)) {
            candidate = saxDelegateResult();
        } else if (StAXResult.class.isAssignableFrom(resultClass)) {
            candidate = staxDelegateResult();
        } else {
            candidate = streamDelegateResult();
        }
        bindDelegateResult(instance, candidate.result());
        if (instance instanceof StreamResult streamResult && candidate.result() instanceof StreamResult delegated) {
            if (streamResult.getWriter() == null && streamResult.getOutputStream() == null) {
                if (delegated.getWriter() != null) {
                    streamResult.setWriter(delegated.getWriter());
                } else if (delegated.getOutputStream() != null) {
                    streamResult.setOutputStream(delegated.getOutputStream());
                }
            }
        } else if (instance instanceof DOMResult domResult && candidate.result() instanceof DOMResult delegated) {
            if (domResult.getNode() == null) {
                domResult.setNode(delegated.getNode());
            }
        } else if (instance instanceof SAXResult saxResult && candidate.result() instanceof SAXResult delegated) {
            if (saxResult.getHandler() == null && delegated.getHandler() != null) {
                saxResult.setHandler(delegated.getHandler());
            }
            if (saxResult.getLexicalHandler() == null && delegated.getLexicalHandler() != null) {
                saxResult.setLexicalHandler(delegated.getLexicalHandler());
            }
        } else if (instance instanceof StAXResult staxResult && candidate.result() instanceof StAXResult delegated) {
            if (staxResult.getXMLStreamWriter() == null && delegated.getXMLStreamWriter() != null) {
                staxResult.setSystemId(delegated.getSystemId());
            }
        }
        pendingMaterializer = candidate.materializer();
        return instance;
    }

    private <T extends Source> T instantiateSourceFromDelegate(Class<T> sourceClass, String xml) throws SQLException {
        if (sourceClass == null) {
            return null;
        }
        try {
            Source streamCandidate = new StreamSource(new StringReader(xml));
            T delegated = instantiateSourceWithDelegate(sourceClass, streamCandidate, xml);
            if (delegated != null) {
                return delegated;
            }

            Source domCandidate = new DOMSource(parseDom(xml));
            delegated = instantiateSourceWithDelegate(sourceClass, domCandidate, xml);
            if (delegated != null) {
                return delegated;
            }

            Source saxCandidate = new SAXSource(new InputSource(new StringReader(xml)));
            delegated = instantiateSourceWithDelegate(sourceClass, saxCandidate, xml);
            if (delegated != null) {
                return delegated;
            }

            XMLInputFactory inputFactory = XMLInputFactory.newFactory();
            Source staxCandidate = new StAXSource(inputFactory.createXMLStreamReader(new StringReader(xml)));
            return instantiateSourceWithDelegate(sourceClass, staxCandidate, xml);
        } catch (SQLException ex) {
            throw ex;
        } catch (Exception ex) {
            throw new SQLException("Failed to build delegated SQLXML source", "HY000", ex);
        }
    }

    private <T extends Source> T instantiateSourceWithDelegate(Class<T> sourceClass, Source delegate, String xml)
            throws SQLException {
        if (sourceClass == null || delegate == null) {
            return null;
        }
        if (sourceClass.isInstance(delegate)) {
            return sourceClass.cast(delegate);
        }
        Constructor<T> ctor = findCompatibleSingleArgConstructor(sourceClass, delegate.getClass());
        if (ctor == null) {
            ctor = findCompatibleSingleArgConstructor(sourceClass, Source.class);
        }
        if (ctor != null) {
            try {
                return ctor.newInstance(delegate);
            } catch (Exception ex) {
                // Continue with factory/no-constructor fallback strategies.
            }
        }

        // Custom wrappers may expose direct payload constructors instead of Source delegates.
        Constructor<T> xmlCtor = findCompatibleSingleArgConstructor(sourceClass, String.class);
        if (xmlCtor == null) {
            xmlCtor = findCompatibleSingleArgConstructor(sourceClass, CharSequence.class);
        }
        if (xmlCtor != null) {
            try {
                return xmlCtor.newInstance(xml == null ? "" : xml);
            } catch (Exception ex) {
                // Continue with additional constructor paths.
            }
        }

        Constructor<T> readerCtor = findCompatibleSingleArgConstructor(sourceClass, Reader.class);
        if (readerCtor != null) {
            try {
                return readerCtor.newInstance(new StringReader(xml == null ? "" : xml));
            } catch (Exception ex) {
                // Continue with additional constructor paths.
            }
        }

        Constructor<T> inputStreamCtor = findCompatibleSingleArgConstructor(sourceClass, InputStream.class);
        if (inputStreamCtor != null) {
            try {
                return inputStreamCtor.newInstance(
                    new ByteArrayInputStream((xml == null ? "" : xml).getBytes(StandardCharsets.UTF_8))
                );
            } catch (Exception ex) {
                // Continue with additional constructor paths.
            }
        }

        Constructor<T> inputSourceCtor = findCompatibleSingleArgConstructor(sourceClass, InputSource.class);
        if (inputSourceCtor != null) {
            try {
                return inputSourceCtor.newInstance(new InputSource(new StringReader(xml == null ? "" : xml)));
            } catch (Exception ex) {
                // Continue with additional constructor paths.
            }
        }

        T factoryInstance = invokeStaticFactory(sourceClass, delegate, Source.class);
        if (factoryInstance != null) {
            return factoryInstance;
        }

        try {
            Constructor<T> noArgCtor = findConstructor(sourceClass);
            T instance = null;
            if (noArgCtor != null) {
                instance = noArgCtor.newInstance();
            }
            if (instance == null) {
                instance = instantiateWithDefaultArguments(sourceClass);
            }
            if (instance == null) {
                return null;
            }
            if (bindDelegateSource(instance, delegate)) {
                return instance;
            }
            return null;
        } catch (Exception ex) {
            throw new SQLException("Failed to instantiate delegated SQLXML source class: " + sourceClass,
                "HY000", ex);
        }
    }

    private <T extends Result> T instantiateResultFromDelegate(Class<T> resultClass) throws SQLException {
        if (resultClass == null) {
            return null;
        }
        CandidateResult streamCandidate = streamDelegateResult();
        T delegated = instantiateResultWithDelegate(resultClass, streamCandidate);
        if (delegated != null) {
            return delegated;
        }

        CandidateResult domCandidate = domDelegateResult();
        delegated = instantiateResultWithDelegate(resultClass, domCandidate);
        if (delegated != null) {
            return delegated;
        }

        CandidateResult saxCandidate = saxDelegateResult();
        delegated = instantiateResultWithDelegate(resultClass, saxCandidate);
        if (delegated != null) {
            return delegated;
        }

        CandidateResult staxCandidate = staxDelegateResult();
        return instantiateResultWithDelegate(resultClass, staxCandidate);
    }

    private <T extends Result> T instantiateResultWithDelegate(Class<T> resultClass, CandidateResult candidate)
            throws SQLException {
        if (resultClass == null || candidate == null || candidate.result() == null) {
            return null;
        }
        if (resultClass.isInstance(candidate.result())) {
            pendingMaterializer = candidate.materializer();
            return resultClass.cast(candidate.result());
        }
        Constructor<T> ctor = findCompatibleSingleArgConstructor(resultClass, candidate.result().getClass());
        if (ctor == null) {
            ctor = findCompatibleSingleArgConstructor(resultClass, Result.class);
        }
        if (ctor != null) {
            try {
                T instance = ctor.newInstance(candidate.result());
                pendingMaterializer = candidate.materializer();
                return instance;
            } catch (Exception ex) {
                // Continue with factory/no-constructor fallback strategies.
            }
        }

        if (candidate.result() instanceof StreamResult streamDelegate) {
            Writer delegateWriter = streamDelegate.getWriter();
            if (delegateWriter != null) {
                Constructor<T> writerCtor = findCompatibleSingleArgConstructor(resultClass, Writer.class);
                if (writerCtor != null) {
                    try {
                        T instance = writerCtor.newInstance(delegateWriter);
                        pendingMaterializer = candidate.materializer();
                        return instance;
                    } catch (Exception ex) {
                        // Continue with additional constructor paths.
                    }
                }
            }
            OutputStream delegateOutput = streamDelegate.getOutputStream();
            if (delegateOutput != null) {
                Constructor<T> outputCtor = findCompatibleSingleArgConstructor(resultClass, OutputStream.class);
                if (outputCtor != null) {
                    try {
                        T instance = outputCtor.newInstance(delegateOutput);
                        pendingMaterializer = candidate.materializer();
                        return instance;
                    } catch (Exception ex) {
                        // Continue with additional constructor paths.
                    }
                }
            }
            StringBuilder builderSink = new StringBuilder();
            Constructor<T> stringBuilderCtor = findCompatibleSingleArgConstructor(resultClass, StringBuilder.class);
            if (stringBuilderCtor != null) {
                try {
                    T instance = stringBuilderCtor.newInstance(builderSink);
                    pendingMaterializer = builderSink::toString;
                    return instance;
                } catch (Exception ex) {
                    // Continue with additional constructor paths.
                }
            }
            Constructor<T> appendableCtor = findCompatibleSingleArgConstructor(resultClass, Appendable.class);
            if (appendableCtor != null) {
                try {
                    T instance = appendableCtor.newInstance(builderSink);
                    pendingMaterializer = builderSink::toString;
                    return instance;
                } catch (Exception ex) {
                    // Continue with additional constructor paths.
                }
            }
        }

        T factoryInstance = invokeStaticFactory(resultClass, candidate.result(), Result.class);
        if (factoryInstance != null) {
            pendingMaterializer = candidate.materializer();
            return factoryInstance;
        }

        try {
            Constructor<T> noArgCtor = findConstructor(resultClass);
            T instance = null;
            if (noArgCtor != null) {
                instance = noArgCtor.newInstance();
            }
            if (instance == null) {
                instance = instantiateWithDefaultArguments(resultClass);
            }
            if (instance == null) {
                return null;
            }
            if (bindDelegateResult(instance, candidate.result())) {
                pendingMaterializer = candidate.materializer();
                return instance;
            }
            return null;
        } catch (Exception ex) {
            throw new SQLException("Failed to instantiate delegated SQLXML result class: " + resultClass,
                "HY000", ex);
        }
    }

    private CandidateResult streamDelegateResult() {
        StringWriter writer = new StringWriter();
        StreamResult result = new StreamResult(writer);
        return new CandidateResult(result, writer::toString);
    }

    private CandidateResult domDelegateResult() {
        DOMResult result = new DOMResult();
        return new CandidateResult(result, () -> serializeDom(result.getNode()));
    }

    private CandidateResult saxDelegateResult() throws SQLException {
        try {
            TransformerFactory transformerFactory = TransformerFactory.newInstance();
            if (transformerFactory instanceof SAXTransformerFactory saxFactory) {
                TransformerHandler handler = saxFactory.newTransformerHandler();
                StringWriter writer = new StringWriter();
                handler.setResult(new StreamResult(writer));
                SAXResult result = new SAXResult(handler);
                return new CandidateResult(result, writer::toString);
            }
            SaxCaptureHandler handler = new SaxCaptureHandler();
            SAXResult result = new SAXResult(handler);
            return new CandidateResult(result, handler::toXml);
        } catch (Exception ex) {
            throw new SQLException("Failed to initialize delegated SAXResult for SQLXML", "HY000", ex);
        }
    }

    private CandidateResult staxDelegateResult() throws SQLException {
        try {
            StringWriter writer = new StringWriter();
            XMLOutputFactory outputFactory = XMLOutputFactory.newFactory();
            XMLStreamWriter streamWriter = outputFactory.createXMLStreamWriter(writer);
            StAXResult result = new StAXResult(streamWriter);
            return new CandidateResult(result, () -> {
                streamWriter.flush();
                streamWriter.close();
                return writer.toString();
            });
        } catch (Exception ex) {
            throw new SQLException("Failed to initialize delegated StAXResult for SQLXML", "HY000", ex);
        }
    }

    private record CandidateResult(Result result, Materializer materializer) {}

    private <T extends Source> T instantiateStreamSourceSubclass(Class<T> sourceClass, String xml) throws SQLException {
        try {
            Constructor<T> ctor;

            ctor = findConstructor(sourceClass);
            if (ctor != null) {
                T instance = ctor.newInstance();
                if (!(instance instanceof StreamSource streamSource)) {
                    throw new SQLFeatureNotSupportedException("Source class not supported: " + sourceClass);
                }
                streamSource.setReader(new StringReader(xml));
                return instance;
            }

            ctor = findConstructor(sourceClass, Reader.class);
            if (ctor != null) {
                return ctor.newInstance(new StringReader(xml));
            }

            ctor = findConstructor(sourceClass, InputStream.class);
            if (ctor != null) {
                return ctor.newInstance(new ByteArrayInputStream(xml.getBytes(StandardCharsets.UTF_8)));
            }

            ctor = findConstructor(sourceClass, Reader.class, String.class);
            if (ctor != null) {
                return ctor.newInstance(new StringReader(xml), "sb:sqlxml");
            }

            ctor = findConstructor(sourceClass, InputStream.class, String.class);
            if (ctor != null) {
                return ctor.newInstance(
                    new ByteArrayInputStream(xml.getBytes(StandardCharsets.UTF_8)),
                    "sb:sqlxml");
            }

            ctor = findConstructor(sourceClass, String.class);
            if (ctor != null) {
                T instance = ctor.newInstance("sb:sqlxml");
                if (instance instanceof StreamSource streamSource) {
                    streamSource.setReader(new StringReader(xml));
                    return instance;
                }
            }

            throw new SQLFeatureNotSupportedException("Source class not supported: " + sourceClass);
        } catch (SQLFeatureNotSupportedException ex) {
            throw ex;
        } catch (Exception ex) {
            throw new SQLFeatureNotSupportedException("Source class not supported: " + sourceClass);
        }
    }

    private <T extends Source> T instantiateDomSourceSubclass(Class<T> sourceClass, String xml) throws SQLException {
        try {
            Node node = parseDom(xml);
            Constructor<T> ctor;

            ctor = findConstructor(sourceClass);
            if (ctor != null) {
                T instance = ctor.newInstance();
                if (!(instance instanceof DOMSource domSource)) {
                    throw new SQLFeatureNotSupportedException("Source class not supported: " + sourceClass);
                }
                domSource.setNode(node);
                return instance;
            }

            ctor = findConstructor(sourceClass, Node.class);
            if (ctor != null) {
                return ctor.newInstance(node);
            }

            ctor = findConstructor(sourceClass, Node.class, String.class);
            if (ctor != null) {
                return ctor.newInstance(node, "sb:sqlxml");
            }

            throw new SQLFeatureNotSupportedException("Source class not supported: " + sourceClass);
        } catch (SQLFeatureNotSupportedException ex) {
            throw ex;
        } catch (Exception ex) {
            throw new SQLFeatureNotSupportedException("Source class not supported: " + sourceClass);
        }
    }

    private <T extends Source> T instantiateSaxSourceSubclass(Class<T> sourceClass, String xml) throws SQLException {
        try {
            InputSource inputSource = new InputSource(new StringReader(xml));
            Constructor<T> ctor;

            ctor = findConstructor(sourceClass);
            if (ctor != null) {
                T instance = ctor.newInstance();
                if (!(instance instanceof SAXSource saxSource)) {
                    throw new SQLFeatureNotSupportedException("Source class not supported: " + sourceClass);
                }
                saxSource.setInputSource(inputSource);
                return instance;
            }

            ctor = findConstructor(sourceClass, InputSource.class);
            if (ctor != null) {
                return ctor.newInstance(inputSource);
            }

            ctor = findConstructor(sourceClass, XMLReader.class, InputSource.class);
            if (ctor != null) {
                return ctor.newInstance(null, inputSource);
            }

            throw new SQLFeatureNotSupportedException("Source class not supported: " + sourceClass);
        } catch (SQLFeatureNotSupportedException ex) {
            throw ex;
        } catch (Exception ex) {
            throw new SQLFeatureNotSupportedException("Source class not supported: " + sourceClass);
        }
    }

    private <T extends Source> T instantiateStaxSourceSubclass(Class<T> sourceClass, String xml) throws SQLException {
        try {
            XMLInputFactory factory = XMLInputFactory.newFactory();
            Constructor<T> ctor = findConstructor(sourceClass, XMLStreamReader.class);
            if (ctor != null) {
                XMLStreamReader reader = factory.createXMLStreamReader(new StringReader(xml));
                return ctor.newInstance(reader);
            }
            ctor = findConstructor(sourceClass, XMLEventReader.class);
            if (ctor != null) {
                XMLEventReader reader = factory.createXMLEventReader(new StringReader(xml));
                return ctor.newInstance(reader);
            }
            throw new SQLFeatureNotSupportedException("Source class not supported: " + sourceClass);
        } catch (SQLFeatureNotSupportedException ex) {
            throw ex;
        } catch (Exception ex) {
            throw new SQLException("Failed to build StAXSource from SQLXML", "HY000", ex);
        }
    }

    private <T extends Result> T instantiateStreamResultSubclass(Class<T> resultClass) throws SQLException {
        try {
            Constructor<T> ctor;
            StringWriter writer = new StringWriter();
            ByteArrayOutputStream output = new ByteArrayOutputStream();

            ctor = findConstructor(resultClass);
            if (ctor != null) {
                T instance = ctor.newInstance();
                if (!(instance instanceof StreamResult streamResult)) {
                    throw new SQLFeatureNotSupportedException("Result class not supported: " + resultClass);
                }
                streamResult.setWriter(writer);
                pendingMaterializer = writer::toString;
                return instance;
            }

            ctor = findConstructor(resultClass, Writer.class);
            if (ctor != null) {
                pendingMaterializer = writer::toString;
                return ctor.newInstance(writer);
            }

            ctor = findConstructor(resultClass, OutputStream.class);
            if (ctor != null) {
                pendingMaterializer = () -> output.toString(StandardCharsets.UTF_8);
                return ctor.newInstance(output);
            }

            ctor = findConstructor(resultClass, String.class);
            if (ctor != null) {
                T instance = ctor.newInstance("sb:sqlxml");
                if (instance instanceof StreamResult streamResult) {
                    streamResult.setWriter(writer);
                    pendingMaterializer = writer::toString;
                    return instance;
                }
            }

            throw new SQLFeatureNotSupportedException("Result class not supported: " + resultClass);
        } catch (SQLFeatureNotSupportedException ex) {
            throw ex;
        } catch (Exception ex) {
            throw new SQLFeatureNotSupportedException("Result class not supported: " + resultClass);
        }
    }

    private <T extends Result> T instantiateDomResultSubclass(Class<T> resultClass) throws SQLException {
        try {
            Constructor<T> ctor;

            ctor = findConstructor(resultClass);
            if (ctor != null) {
                T instance = ctor.newInstance();
                if (!(instance instanceof DOMResult domResult)) {
                    throw new SQLFeatureNotSupportedException("Result class not supported: " + resultClass);
                }
                pendingMaterializer = () -> serializeDom(domResult.getNode());
                return instance;
            }

            ctor = findConstructor(resultClass, Node.class);
            if (ctor != null) {
                DOMResult domResult = (DOMResult) ctor.newInstance((Node) null);
                pendingMaterializer = () -> serializeDom(domResult.getNode());
                @SuppressWarnings("unchecked")
                T instance = (T) domResult;
                return instance;
            }

            ctor = findConstructor(resultClass, Node.class, String.class);
            if (ctor != null) {
                DOMResult domResult = (DOMResult) ctor.newInstance(null, "sb:sqlxml");
                pendingMaterializer = () -> serializeDom(domResult.getNode());
                @SuppressWarnings("unchecked")
                T instance = (T) domResult;
                return instance;
            }

            throw new SQLFeatureNotSupportedException("Result class not supported: " + resultClass);
        } catch (SQLFeatureNotSupportedException ex) {
            throw ex;
        } catch (Exception ex) {
            throw new SQLFeatureNotSupportedException("Result class not supported: " + resultClass);
        }
    }

    private <T extends Result> T instantiateSaxResultSubclass(Class<T> resultClass) throws SQLException {
        try {
            Constructor<T> ctor;
            T instance;

            ctor = findConstructor(resultClass);
            if (ctor != null) {
                instance = ctor.newInstance();
            } else {
                ctor = findConstructor(resultClass, org.xml.sax.ContentHandler.class);
                if (ctor != null) {
                    TransformerFactory transformerFactory = TransformerFactory.newInstance();
                    if (transformerFactory instanceof SAXTransformerFactory saxFactory) {
                        TransformerHandler handler = saxFactory.newTransformerHandler();
                        StringWriter writer = new StringWriter();
                        handler.setResult(new StreamResult(writer));
                        pendingMaterializer = writer::toString;
                        return ctor.newInstance(handler);
                    }
                    SaxCaptureHandler handler = new SaxCaptureHandler();
                    pendingMaterializer = handler::toXml;
                    return ctor.newInstance(handler);
                }
                throw new SQLFeatureNotSupportedException("Result class not supported: " + resultClass);
            }

            if (!(instance instanceof SAXResult saxResult)) {
                throw new SQLFeatureNotSupportedException("Result class not supported: " + resultClass);
            }
            TransformerFactory transformerFactory = TransformerFactory.newInstance();
            if (transformerFactory instanceof SAXTransformerFactory saxFactory) {
                TransformerHandler handler = saxFactory.newTransformerHandler();
                StringWriter writer = new StringWriter();
                handler.setResult(new StreamResult(writer));
                saxResult.setHandler(handler);
                pendingMaterializer = writer::toString;
                return instance;
            }
            SaxCaptureHandler handler = new SaxCaptureHandler();
            saxResult.setHandler(handler);
            pendingMaterializer = handler::toXml;
            return instance;
        } catch (SQLFeatureNotSupportedException ex) {
            throw ex;
        } catch (Exception ex) {
            throw new SQLFeatureNotSupportedException("Result class not supported: " + resultClass);
        }
    }

    private <T extends Result> T instantiateStaxResultSubclass(Class<T> resultClass) throws SQLException {
        try {
            StringWriter writer = new StringWriter();
            XMLOutputFactory outputFactory = XMLOutputFactory.newFactory();
            XMLStreamWriter streamWriter = outputFactory.createXMLStreamWriter(writer);
            Constructor<T> ctor = findConstructor(resultClass, XMLStreamWriter.class);
            if (ctor != null) {
                pendingMaterializer = () -> {
                    streamWriter.flush();
                    streamWriter.close();
                    return writer.toString();
                };
                return ctor.newInstance(streamWriter);
            }

            XMLEventWriter eventWriter = outputFactory.createXMLEventWriter(writer);
            ctor = findConstructor(resultClass, XMLEventWriter.class);
            if (ctor != null) {
                pendingMaterializer = () -> {
                    eventWriter.flush();
                    eventWriter.close();
                    return writer.toString();
                };
                return ctor.newInstance(eventWriter);
            }

            throw new SQLFeatureNotSupportedException("Result class not supported: " + resultClass);
        } catch (Exception ex) {
            if (ex instanceof SQLFeatureNotSupportedException) {
                throw (SQLFeatureNotSupportedException) ex;
            }
            throw new SQLException("Failed to initialize StAXResult for SQLXML", "HY000", ex);
        }
    }

    private static <T> Constructor<T> findConstructor(Class<T> type, Class<?>... parameterTypes) {
        try {
            Constructor<T> ctor = type.getDeclaredConstructor(parameterTypes);
            ctor.setAccessible(true);
            return ctor;
        } catch (NoSuchMethodException ex) {
            return null;
        }
    }

    private static <T> Constructor<T> findCompatibleSingleArgConstructor(Class<T> type, Class<?> argType) {
        Constructor<?>[] constructors = type.getDeclaredConstructors();
        for (Constructor<?> constructor : constructors) {
            Class<?>[] params = constructor.getParameterTypes();
            if (params.length != 1) {
                continue;
            }
            if (!params[0].isAssignableFrom(argType)) {
                continue;
            }
            constructor.setAccessible(true);
            @SuppressWarnings("unchecked")
            Constructor<T> compatible = (Constructor<T>) constructor;
            return compatible;
        }
        return null;
    }

    private static <T> T instantiateWithDefaultArguments(Class<T> type) {
        if (type == null) {
            return null;
        }
        Constructor<?>[] constructors = type.getDeclaredConstructors();
        Constructor<?> best = null;
        for (Constructor<?> constructor : constructors) {
            if (best == null || constructor.getParameterCount() < best.getParameterCount()) {
                best = constructor;
            }
        }
        if (best == null) {
            return null;
        }
        try {
            best.setAccessible(true);
            Class<?>[] parameterTypes = best.getParameterTypes();
            Object[] args = new Object[parameterTypes.length];
            for (int i = 0; i < parameterTypes.length; i++) {
                args[i] = defaultValue(parameterTypes[i]);
            }
            @SuppressWarnings("unchecked")
            T instance = (T) best.newInstance(args);
            return instance;
        } catch (Exception ex) {
            return allocateWithUnsafe(type);
        }
    }

    private static Object defaultValue(Class<?> type) {
        if (type == null || !type.isPrimitive()) {
            return null;
        }
        if (type == boolean.class) return false;
        if (type == byte.class) return (byte) 0;
        if (type == short.class) return (short) 0;
        if (type == int.class) return 0;
        if (type == long.class) return 0L;
        if (type == float.class) return 0f;
        if (type == double.class) return 0d;
        if (type == char.class) return '\0';
        return null;
    }

    private static <T> T adaptSourceInterface(Class<T> sourceClass, Source delegate) {
        if (sourceClass == null || delegate == null || !sourceClass.isInterface()) {
            return null;
        }
        InvocationHandler handler = (proxy, method, args) -> invokeDelegatingInterfaceMethod(
            proxy, method, args, delegate, Source.class
        );
        Object proxy = Proxy.newProxyInstance(
            sourceClass.getClassLoader(),
            new Class<?>[]{sourceClass},
            handler
        );
        return sourceClass.cast(proxy);
    }

    private static <T> T adaptResultInterface(Class<T> resultClass, Result delegate) {
        if (resultClass == null || delegate == null || !resultClass.isInterface()) {
            return null;
        }
        InvocationHandler handler = (proxy, method, args) -> invokeDelegatingInterfaceMethod(
            proxy, method, args, delegate, Result.class
        );
        Object proxy = Proxy.newProxyInstance(
            resultClass.getClassLoader(),
            new Class<?>[]{resultClass},
            handler
        );
        return resultClass.cast(proxy);
    }

    private static Object invokeDelegatingInterfaceMethod(
            Object proxy, Method method, Object[] args, Object delegate, Class<?> baseType) throws Throwable {
        if (method == null) {
            return null;
        }
        String name = method.getName();
        if ("toString".equals(name) && method.getParameterCount() == 0) {
            return delegate.toString();
        }
        if ("hashCode".equals(name) && method.getParameterCount() == 0) {
            return delegate.hashCode();
        }
        if ("equals".equals(name) && method.getParameterCount() == 1) {
            return proxy == args[0];
        }
        if ("delegate".equals(name) && method.getParameterCount() == 0
            && method.getReturnType().isAssignableFrom(delegate.getClass())) {
            return delegate;
        }
        Method delegateMethod = findDelegateMethod(delegate.getClass(), method);
        if (delegateMethod != null) {
            delegateMethod.setAccessible(true);
            return delegateMethod.invoke(delegate, args);
        }
        if (method.getReturnType() == void.class) {
            return null;
        }
        return defaultValue(method.getReturnType());
    }

    private static Method findDelegateMethod(Class<?> delegateClass, Method interfaceMethod) {
        if (delegateClass == null || interfaceMethod == null) {
            return null;
        }
        try {
            return delegateClass.getMethod(interfaceMethod.getName(), interfaceMethod.getParameterTypes());
        } catch (NoSuchMethodException ex) {
            try {
                return delegateClass.getDeclaredMethod(interfaceMethod.getName(), interfaceMethod.getParameterTypes());
            } catch (NoSuchMethodException ignored) {
                return null;
            }
        }
    }

    @SuppressWarnings("unchecked")
    private static <T> T allocateWithUnsafe(Class<T> type) {
        try {
            Class<?> unsafeClass = Class.forName("sun.misc.Unsafe");
            Field theUnsafe = unsafeClass.getDeclaredField("theUnsafe");
            theUnsafe.setAccessible(true);
            Object unsafe = theUnsafe.get(null);
            Method allocateInstance = unsafeClass.getMethod("allocateInstance", Class.class);
            Object instance = allocateInstance.invoke(unsafe, type);
            if (instance == null) {
                return null;
            }
            return (T) instance;
        } catch (Exception ex) {
            return null;
        }
    }

    private static <T> T invokeStaticFactory(Class<T> targetClass, Object delegate, Class<?> delegateSuperType)
            throws SQLException {
        if (targetClass == null || delegate == null) {
            return null;
        }
        String[] factoryNames = {
            "of",
            "from",
            "valueOf",
            "newInstance",
            "create",
            "createInstance",
            "fromSource",
            "fromResult"
        };
        for (String factoryName : factoryNames) {
            T created = invokeStaticFactoryByArgType(targetClass, factoryName, delegate, delegate.getClass());
            if (created != null) {
                return created;
            }
            if (delegateSuperType != null) {
                created = invokeStaticFactoryByArgType(targetClass, factoryName, delegate, delegateSuperType);
                if (created != null) {
                    return created;
                }
            }
        }
        return null;
    }

    private static <T> T invokeStaticFactoryByArgType(Class<T> targetClass, String methodName, Object delegate,
                                                      Class<?> argumentType) throws SQLException {
        if (targetClass == null || methodName == null || delegate == null || argumentType == null) {
            return null;
        }
        for (Method method : targetClass.getMethods()) {
            T value = invokeFactoryMethodIfCompatible(targetClass, methodName, delegate, argumentType, method);
            if (value != null) {
                return value;
            }
        }
        for (Method method : targetClass.getDeclaredMethods()) {
            T value = invokeFactoryMethodIfCompatible(targetClass, methodName, delegate, argumentType, method);
            if (value != null) {
                return value;
            }
        }
        return null;
    }

    private static <T> T invokeFactoryMethodIfCompatible(Class<T> targetClass, String methodName, Object delegate,
                                                          Class<?> argumentType, Method method) throws SQLException {
        if (method == null
            || !method.getName().equals(methodName)
            || !java.lang.reflect.Modifier.isStatic(method.getModifiers())
            || method.getParameterCount() != 1
            || !targetClass.isAssignableFrom(method.getReturnType())) {
            return null;
        }
        Class<?> parameterType = method.getParameterTypes()[0];
        if (!parameterType.isAssignableFrom(argumentType)) {
            return null;
        }
        try {
            method.setAccessible(true);
            Object value = method.invoke(null, delegate);
            return targetClass.cast(value);
        } catch (Exception ex) {
            throw new SQLException("Failed to invoke delegated SQLXML factory method " + methodName
                + " on " + targetClass.getName(), "HY000", ex);
        }
    }

    private boolean bindDelegateSource(Object target, Source delegate) {
        boolean bound = false;
        bound |= invokeCompatibleSetter(target, "setSource", delegate);
        bound |= invokeCompatibleSetter(target, "setDelegate", delegate);
        bound |= bindCompatibleField(target, delegate);
        bound |= invokeBestEffortDelegateMutator(target, delegate, "delegate", "source", "wrapped", "bind");
        if (delegate instanceof StreamSource streamSource) {
            Reader reader = streamSource.getReader();
            if (reader != null) {
                bound |= invokeCompatibleSetter(target, "setReader", reader);
            }
            InputStream inputStream = streamSource.getInputStream();
            if (inputStream != null) {
                bound |= invokeCompatibleSetter(target, "setInputStream", inputStream);
            }
            if (streamSource.getSystemId() != null) {
                bound |= invokeCompatibleSetter(target, "setSystemId", streamSource.getSystemId());
            }
        } else if (delegate instanceof DOMSource domSource) {
            if (domSource.getNode() != null) {
                bound |= invokeCompatibleSetter(target, "setNode", domSource.getNode());
            }
            if (domSource.getSystemId() != null) {
                bound |= invokeCompatibleSetter(target, "setSystemId", domSource.getSystemId());
            }
        } else if (delegate instanceof SAXSource saxSource) {
            if (saxSource.getInputSource() != null) {
                bound |= invokeCompatibleSetter(target, "setInputSource", saxSource.getInputSource());
            }
            if (saxSource.getXMLReader() != null) {
                bound |= invokeCompatibleSetter(target, "setXMLReader", saxSource.getXMLReader());
            }
            if (saxSource.getSystemId() != null) {
                bound |= invokeCompatibleSetter(target, "setSystemId", saxSource.getSystemId());
            }
        } else if (delegate instanceof StAXSource staxSource) {
            if (staxSource.getXMLStreamReader() != null) {
                bound |= invokeCompatibleSetter(target, "setXMLStreamReader", staxSource.getXMLStreamReader());
            }
            if (staxSource.getXMLEventReader() != null) {
                bound |= invokeCompatibleSetter(target, "setXMLEventReader", staxSource.getXMLEventReader());
            }
            if (staxSource.getSystemId() != null) {
                bound |= invokeCompatibleSetter(target, "setSystemId", staxSource.getSystemId());
            }
        }
        if (target instanceof Source source && delegate.getSystemId() != null) {
            source.setSystemId(delegate.getSystemId());
            bound = true;
        }
        if (delegate.getSystemId() != null) {
            bound |= bindNamedStringField(target, "systemId", delegate.getSystemId());
        }
        return bound;
    }

    private boolean bindDelegateResult(Object target, Result delegate) {
        boolean bound = false;
        bound |= invokeCompatibleSetter(target, "setResult", delegate);
        bound |= invokeCompatibleSetter(target, "setDelegate", delegate);
        bound |= bindCompatibleField(target, delegate);
        bound |= invokeBestEffortDelegateMutator(target, delegate, "delegate", "result", "wrapped", "bind");
        if (delegate instanceof StreamResult streamResult) {
            if (streamResult.getWriter() != null) {
                bound |= invokeCompatibleSetter(target, "setWriter", streamResult.getWriter());
            }
            if (streamResult.getOutputStream() != null) {
                bound |= invokeCompatibleSetter(target, "setOutputStream", streamResult.getOutputStream());
            }
            if (streamResult.getSystemId() != null) {
                bound |= invokeCompatibleSetter(target, "setSystemId", streamResult.getSystemId());
            }
        } else if (delegate instanceof DOMResult domResult) {
            if (domResult.getNode() != null) {
                bound |= invokeCompatibleSetter(target, "setNode", domResult.getNode());
            }
            if (domResult.getSystemId() != null) {
                bound |= invokeCompatibleSetter(target, "setSystemId", domResult.getSystemId());
            }
        } else if (delegate instanceof SAXResult saxResult) {
            if (saxResult.getHandler() != null) {
                bound |= invokeCompatibleSetter(target, "setHandler", saxResult.getHandler());
            }
            if (saxResult.getLexicalHandler() != null) {
                bound |= invokeCompatibleSetter(target, "setLexicalHandler", saxResult.getLexicalHandler());
            }
            if (saxResult.getSystemId() != null) {
                bound |= invokeCompatibleSetter(target, "setSystemId", saxResult.getSystemId());
            }
        } else if (delegate instanceof StAXResult staxResult) {
            if (staxResult.getXMLStreamWriter() != null) {
                bound |= invokeCompatibleSetter(target, "setXMLStreamWriter", staxResult.getXMLStreamWriter());
            }
            if (staxResult.getXMLEventWriter() != null) {
                bound |= invokeCompatibleSetter(target, "setXMLEventWriter", staxResult.getXMLEventWriter());
            }
            if (staxResult.getSystemId() != null) {
                bound |= invokeCompatibleSetter(target, "setSystemId", staxResult.getSystemId());
            }
        }
        if (target instanceof Result result && delegate.getSystemId() != null) {
            result.setSystemId(delegate.getSystemId());
            bound = true;
        }
        if (delegate.getSystemId() != null) {
            bound |= bindNamedStringField(target, "systemId", delegate.getSystemId());
        }
        return bound;
    }

    private boolean bindCompatibleField(Object target, Object value) {
        if (target == null || value == null) {
            return false;
        }
        Class<?> type = target.getClass();
        while (type != null && type != Object.class) {
            for (Field field : type.getDeclaredFields()) {
                if (java.lang.reflect.Modifier.isStatic(field.getModifiers())) {
                    continue;
                }
                if (!field.getType().isAssignableFrom(value.getClass())) {
                    continue;
                }
                try {
                    field.setAccessible(true);
                    field.set(target, value);
                    return true;
                } catch (Exception ignored) {
                    // Try next candidate field.
                }
            }
            type = type.getSuperclass();
        }
        return false;
    }

    private boolean bindNamedStringField(Object target, String fieldName, String value) {
        if (target == null || fieldName == null || fieldName.isEmpty() || value == null) {
            return false;
        }
        Class<?> type = target.getClass();
        while (type != null && type != Object.class) {
            try {
                Field field = type.getDeclaredField(fieldName);
                if (!String.class.equals(field.getType())
                    || java.lang.reflect.Modifier.isStatic(field.getModifiers())) {
                    return false;
                }
                field.setAccessible(true);
                field.set(target, value);
                return true;
            } catch (NoSuchFieldException ex) {
                type = type.getSuperclass();
            } catch (Exception ignored) {
                return false;
            }
        }
        return false;
    }

    private boolean invokeCompatibleSetter(Object target, String methodName, Object argument) {
        if (target == null || argument == null || methodName == null || methodName.isEmpty()) {
            return false;
        }
        Class<?> type = target.getClass();
        for (Method method : type.getMethods()) {
            if (!method.getName().equals(methodName) || method.getParameterCount() != 1) {
                continue;
            }
            Class<?> parameterType = method.getParameterTypes()[0];
            if (!parameterType.isAssignableFrom(argument.getClass())) {
                continue;
            }
            try {
                method.setAccessible(true);
                method.invoke(target, argument);
                return true;
            } catch (Exception ignored) {
                // Try another compatible method.
            }
        }
        for (Method method : type.getDeclaredMethods()) {
            if (!method.getName().equals(methodName) || method.getParameterCount() != 1) {
                continue;
            }
            Class<?> parameterType = method.getParameterTypes()[0];
            if (!parameterType.isAssignableFrom(argument.getClass())) {
                continue;
            }
            try {
                method.setAccessible(true);
                method.invoke(target, argument);
                return true;
            } catch (Exception ignored) {
                // Try another compatible method.
            }
        }
        return false;
    }

    private boolean invokeBestEffortDelegateMutator(Object target, Object argument, String... nameHints) {
        if (target == null || argument == null) {
            return false;
        }
        List<Method> methods = new ArrayList<>();
        methods.addAll(Arrays.asList(target.getClass().getMethods()));
        methods.addAll(Arrays.asList(target.getClass().getDeclaredMethods()));
        for (Method method : methods) {
            if (method == null
                || java.lang.reflect.Modifier.isStatic(method.getModifiers())
                || method.getParameterCount() != 1) {
                continue;
            }
            String methodName = method.getName() == null ? "" : method.getName().toLowerCase(Locale.ROOT);
            if (!matchesNameHint(methodName, nameHints)) {
                continue;
            }
            Class<?> parameterType = method.getParameterTypes()[0];
            if (!parameterType.isAssignableFrom(argument.getClass())) {
                continue;
            }
            Class<?> returnType = method.getReturnType();
            if (!(Void.TYPE.equals(returnType) || returnType.isAssignableFrom(target.getClass()))) {
                continue;
            }
            try {
                method.setAccessible(true);
                method.invoke(target, argument);
                return true;
            } catch (Exception ignored) {
                // Keep searching for another compatible mutator.
            }
        }
        return false;
    }

    private boolean matchesNameHint(String methodName, String... nameHints) {
        if (methodName == null || methodName.isBlank()) {
            return false;
        }
        if (nameHints == null || nameHints.length == 0) {
            return false;
        }
        for (String hint : nameHints) {
            if (hint == null || hint.isBlank()) {
                continue;
            }
            if (methodName.contains(hint.toLowerCase(Locale.ROOT))) {
                return true;
            }
        }
        return false;
    }

    private void materializeIfPending() throws SQLException {
        if (pendingMaterializer == null) {
            return;
        }
        try {
            data = pendingMaterializer.materialize();
            pendingMaterializer = null;
        } catch (SQLException ex) {
            throw ex;
        } catch (Exception ex) {
            throw new SQLException("Failed to materialize SQLXML data", "HY000", ex);
        }
    }

    private Node parseDom(String xml) throws SQLException {
        try {
            DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
            factory.setNamespaceAware(true);
            factory.setFeature(XMLConstants.FEATURE_SECURE_PROCESSING, true);
            disableExternalEntities(factory);
            DocumentBuilder builder = factory.newDocumentBuilder();
            return builder.parse(new InputSource(new StringReader(xml)));
        } catch (Exception ex) {
            throw new SQLException("Failed to parse SQLXML DOM source", "22000", ex);
        }
    }

    private String serializeDom(Node node) throws SQLException {
        if (node == null) {
            return null;
        }
        try {
            Transformer transformer = TransformerFactory.newInstance().newTransformer();
            StringWriter writer = new StringWriter();
            transformer.transform(new DOMSource(node), new StreamResult(writer));
            return writer.toString();
        } catch (Exception ex) {
            throw new SQLException("Failed to serialize SQLXML DOM result", "22000", ex);
        }
    }

    private void disableExternalEntities(DocumentBuilderFactory factory) {
        try {
            factory.setFeature("http://apache.org/xml/features/disallow-doctype-decl", true);
        } catch (Exception ignored) {
            // Parser does not expose this hardening switch.
        }
        try {
            factory.setFeature("http://xml.org/sax/features/external-general-entities", false);
        } catch (Exception ignored) {
            // Parser does not expose this hardening switch.
        }
        try {
            factory.setFeature("http://xml.org/sax/features/external-parameter-entities", false);
        } catch (Exception ignored) {
            // Parser does not expose this hardening switch.
        }
    }

    private void checkFreed() throws SQLException {
        if (freed) {
            throw new SQLException("SQLXML has been freed", "HY000");
        }
    }

    private static final class SaxCaptureHandler extends DefaultHandler {
        private final StringBuilder builder = new StringBuilder();
        private boolean startedDocument;

        @Override
        public void startDocument() {
            startedDocument = true;
        }

        @Override
        public void startElement(String uri, String localName, String qName, Attributes attributes) {
            String element = localName != null && !localName.isEmpty() ? localName : qName;
            builder.append('<').append(element);
            if (attributes != null) {
                for (int i = 0; i < attributes.getLength(); i++) {
                    String name = attributes.getQName(i);
                    if (name == null || name.isEmpty()) {
                        name = attributes.getLocalName(i);
                    }
                    builder.append(' ').append(name).append("=\"")
                        .append(escapeXml(attributes.getValue(i))).append('"');
                }
            }
            builder.append('>');
        }

        @Override
        public void characters(char[] ch, int start, int length) {
            if (length <= 0) {
                return;
            }
            builder.append(escapeXml(new String(ch, start, length)));
        }

        @Override
        public void endElement(String uri, String localName, String qName) {
            String element = localName != null && !localName.isEmpty() ? localName : qName;
            builder.append("</").append(element).append('>');
        }

        private String toXml() {
            if (!startedDocument && builder.length() == 0) {
                return null;
            }
            return builder.toString();
        }
    }

    private static String escapeXml(String value) {
        if (value == null || value.isEmpty()) {
            return "";
        }
        StringBuilder out = new StringBuilder(value.length());
        for (int i = 0; i < value.length(); i++) {
            char ch = value.charAt(i);
            switch (ch) {
                case '&' -> out.append("&amp;");
                case '<' -> out.append("&lt;");
                case '>' -> out.append("&gt;");
                case '"' -> out.append("&quot;");
                case '\'' -> out.append("&apos;");
                default -> out.append(ch);
            }
        }
        return out.toString();
    }
}
