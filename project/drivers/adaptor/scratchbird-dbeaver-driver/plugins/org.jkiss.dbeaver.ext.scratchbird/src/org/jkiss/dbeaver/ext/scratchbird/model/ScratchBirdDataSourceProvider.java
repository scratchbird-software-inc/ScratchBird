// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * DBeaver - Universal Database Manager
 * Copyright (C) 2010-2026 DBeaver Corp and others
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.jkiss.dbeaver.ext.scratchbird.model;

import org.jkiss.code.NotNull;
import org.jkiss.dbeaver.DBException;
import org.jkiss.dbeaver.model.DBConstants;
import org.jkiss.dbeaver.model.DBPDataSource;
import org.jkiss.dbeaver.model.DBPDataSourceContainer;
import org.jkiss.dbeaver.model.DBPDataSourceProvider;
import org.jkiss.dbeaver.ext.generic.GenericConstants;
import org.jkiss.dbeaver.ext.generic.GenericMetaModelRegistry;
import org.jkiss.dbeaver.ext.generic.model.meta.GenericMetaModel;
import org.jkiss.dbeaver.model.DatabaseURL;
import org.jkiss.dbeaver.model.app.DBPPlatform;
import org.jkiss.dbeaver.model.connection.DBPConnectionConfiguration;
import org.jkiss.dbeaver.model.connection.DBPDriver;
import org.jkiss.dbeaver.model.connection.DBPDriverConfigurationType;
import org.jkiss.dbeaver.model.impl.PropertyDescriptor;
import org.jkiss.dbeaver.model.messages.ModelMessages;
import org.jkiss.dbeaver.model.preferences.DBPPropertyDescriptor;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;
import org.jkiss.utils.CommonUtils;

import java.sql.Driver;
import java.sql.DriverPropertyInfo;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Properties;

public class ScratchBirdDataSourceProvider implements DBPDataSourceProvider {

    @Override
    public void init(@NotNull DBPPlatform platform) {
        // No provider-global initialization is required for ScratchBird.
    }

    @NotNull
    @SuppressWarnings("unused")
    public Class<? extends DBPDataSource> getDataSourceClass() {
        return ScratchBirdDataSource.class;
    }

    @Override
    public long getFeatures() {
        return FEATURE_CATALOGS | FEATURE_SCHEMAS;
    }

    @NotNull
    @Override
    public String getConnectionURL(@NotNull DBPDriver driver, @NotNull DBPConnectionConfiguration connectionInfo) {
        if (connectionInfo.getConfigurationType() == DBPDriverConfigurationType.URL
            && !CommonUtils.isEmpty(connectionInfo.getUrl())) {
            return connectionInfo.getUrl();
        }
        return DatabaseURL.generateUrlByTemplate(driver, connectionInfo);
    }

    @NotNull
    @Override
    public DBPDataSource openDataSource(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBPDataSourceContainer container
    ) throws DBException {
        GenericMetaModel metaModel = GenericMetaModelRegistry.getInstance().getMetaModel(container);
        return metaModel.createDataSourceImpl(monitor, container);
    }

    @NotNull
    @Override
    public DBPPropertyDescriptor[] getConnectionProperties(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBPDriver driver,
        @NotNull DBPConnectionConfiguration connectionInfo
    ) throws DBException {
        DBPPropertyDescriptor[] connectionProperties = readDriverProperties(monitor, driver, connectionInfo);
        if (connectionProperties == null || connectionProperties.length == 0) {
            String driverParametersString = CommonUtils.toString(
                driver.getDriverParameter(GenericConstants.PARAM_DRIVER_PROPERTIES)
            );
            if (!driverParametersString.isEmpty()) {
                String[] propList = driverParametersString.split(",");
                connectionProperties = new DBPPropertyDescriptor[propList.length];
                for (int i = 0; i < propList.length; i++) {
                    String propName = propList[i].trim();
                    connectionProperties[i] = new PropertyDescriptor(
                        ModelMessages.model_jdbc_driver_properties,
                        propName,
                        propName,
                        ScratchBirdSecurityRedactor.sanitizeDescription(propName, null),
                        String.class,
                        false,
                        null,
                        null,
                        true
                    );
                }
            }
        }
        return connectionProperties == null ? new DBPPropertyDescriptor[0] : connectionProperties;
    }

    private DBPPropertyDescriptor[] readDriverProperties(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBPDriver driver,
        @NotNull DBPConnectionConfiguration connectionInfo
    ) throws DBException {
        if (driver.isInternalDriver()) {
            return null;
        }

        Object driverInstance = driver.getDefaultDriverLoader().getDriverInstance(monitor);
        if (!(driverInstance instanceof Driver jdbcDriver)) {
            return null;
        }

        Properties driverProps = new Properties();
        if (driver.isPropagateDriverProperties()) {
            driverProps.putAll(connectionInfo.getProperties());
        }

        final DriverPropertyInfo[] propDescs;
        try {
            propDescs = jdbcDriver.getPropertyInfo(connectionInfo.getUrl(), driverProps);
        } catch (Throwable e) {
            return null;
        }
        if (propDescs == null) {
            return null;
        }

        List<DBPPropertyDescriptor> properties = new ArrayList<>();
        for (DriverPropertyInfo desc : propDescs) {
            if (desc == null
                || DBConstants.DATA_SOURCE_PROPERTY_USER.equals(desc.name)
                || DBConstants.DATA_SOURCE_PROPERTY_PASSWORD.equals(desc.name)) {
                continue;
            }
            properties.add(new PropertyDescriptor(
                ModelMessages.model_jdbc_driver_properties,
                desc.name,
                desc.name,
                ScratchBirdSecurityRedactor.sanitizeDescription(desc.name, desc.description),
                String.class,
                desc.required,
                ScratchBirdSecurityRedactor.sanitizeDefaultValue(desc.name, desc.value),
                ScratchBirdSecurityRedactor.isSensitiveProperty(desc.name) ? null : desc.choices,
                true
            ));
        }
        return properties.toArray(new DBPPropertyDescriptor[0]);
    }
}
