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
package org.jkiss.dbeaver.ext.scratchbird.model.edit;

import org.jkiss.code.NotNull;
import org.jkiss.code.Nullable;
import org.jkiss.dbeaver.DBException;
import org.jkiss.dbeaver.ext.generic.model.GenericDataSourceInfo;
import org.jkiss.dbeaver.ext.generic.model.GenericProcedure;
import org.jkiss.dbeaver.ext.generic.model.GenericTableColumn;
import org.jkiss.dbeaver.ext.generic.model.GenericTableConstraintColumn;
import org.jkiss.dbeaver.ext.generic.model.GenericStructContainer;
import org.jkiss.dbeaver.ext.generic.model.GenericTableBase;
import org.jkiss.dbeaver.ext.generic.model.GenericTableTrigger;
import org.jkiss.dbeaver.ext.generic.model.GenericUniqueKey;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdDataSource;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdNamespaceSemantics;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdObjectFormContext;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdSchemaNode;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdTable;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdView;
import org.jkiss.dbeaver.model.DBPDataSource;
import org.jkiss.dbeaver.model.DBPEvaluationContext;
import org.jkiss.dbeaver.model.DBPQualifiedObject;
import org.jkiss.dbeaver.model.DBUtils;
import org.jkiss.dbeaver.model.struct.DBSEntityConstraintType;
import org.jkiss.dbeaver.model.struct.DBSObject;
import org.jkiss.utils.CommonUtils;

import java.util.List;

final class ScratchBirdManagerSupport {

    private ScratchBirdManagerSupport() {
    }

    static boolean isScratchBirdDataSource(@Nullable DBPDataSource dataSource) {
        return dataSource instanceof ScratchBirdDataSource;
    }

    static boolean isScratchBirdContainer(@Nullable Object container) {
        if (container instanceof GenericStructContainer structContainer) {
            return isScratchBirdDataSource(structContainer.getDataSource());
        }
        if (container instanceof GenericTableBase tableBase) {
            return isScratchBirdDataSource(tableBase.getDataSource());
        }
        return false;
    }

    static boolean canCreateRegularSqlObject(@NotNull Object container) {
        if (!(container instanceof GenericStructContainer structContainer)) {
            return false;
        }
        if (!isScratchBirdDataSource(structContainer.getDataSource())) {
            return false;
        }
        if (container instanceof ScratchBirdSchemaNode schemaNode && schemaNode.isClientOnly()) {
            return false;
        }
        String path = containerPath(structContainer);
        return !ScratchBirdNamespaceSemantics.isMetricsPath(path) &&
            !ScratchBirdNamespaceSemantics.isDomainPath(path);
    }

    static boolean canCreateDomain(@NotNull Object container) {
        return container instanceof GenericStructContainer structContainer &&
            isScratchBirdDataSource(structContainer.getDataSource()) &&
            !(container instanceof ScratchBirdSchemaNode schemaNode && schemaNode.isClientOnly()) &&
            ScratchBirdNamespaceSemantics.isDomainPath(containerPath(structContainer));
    }

    static boolean canCreateStoredCode(@NotNull Object container) {
        return container instanceof GenericStructContainer structContainer &&
            canCreateRegularSqlObject(container) &&
            structContainer.getDataSource().getInfo().supportsStoredCode();
    }

    static boolean canCreateTableTrigger(@NotNull Object container) {
        return container instanceof GenericTableBase tableBase &&
            isScratchBirdDataSource(tableBase.getDataSource()) &&
            !tableBase.isView() &&
            tableBase.getDataSource().getMetaModel().supportsTriggers(tableBase.getDataSource());
    }

    static boolean canCreateConstraint(@NotNull Object container) {
        return container instanceof GenericTableBase tableBase &&
            isScratchBirdDataSource(tableBase.getDataSource()) &&
            !tableBase.isView() &&
            canCreateRegularSqlObject(tableBase.getContainer()) &&
            (!(tableBase.getDataSource().getInfo() instanceof GenericDataSourceInfo dataSourceInfo) ||
                dataSourceInfo.supportsTableConstraints());
    }

    static boolean canCreateForeignKey(@NotNull Object container) {
        return container instanceof GenericTableBase tableBase &&
            isScratchBirdDataSource(tableBase.getDataSource()) &&
            !tableBase.isView() &&
            canCreateRegularSqlObject(tableBase.getContainer()) &&
            tableBase.getDataSource().getInfo().supportsReferentialIntegrity();
    }

    static boolean canCreateIndex(@NotNull Object container) {
        return container instanceof GenericTableBase tableBase &&
            isScratchBirdDataSource(tableBase.getDataSource()) &&
            !tableBase.isView() &&
            canCreateRegularSqlObject(tableBase.getContainer()) &&
            tableBase.getDataSource().getInfo().supportsIndexes() &&
            tableBase.getDataSource().getSQLDialect().supportsIndexCreateAndDrop();
    }

    static boolean canDeleteObject(@NotNull DBSObject object) {
        if (!isScratchBirdDataSource(object.getDataSource())) {
            return true;
        }
        String path = objectPath(object);
        return path == null || !ScratchBirdNamespaceSemantics.isMetricsPath(path);
    }

    static boolean isNonRelational(@NotNull GenericTableBase tableBase) {
        return ScratchBirdObjectFormContext.isLikelyNonRelationalTable(tableBase);
    }

    @NotNull
    static GenericTableColumn firstTableColumn(
        @NotNull org.jkiss.dbeaver.model.runtime.DBRProgressMonitor monitor,
        @NotNull GenericTableBase tableBase
    ) throws DBException {
        List<? extends GenericTableColumn> columns = tableBase.getAttributes(monitor);
        if (CommonUtils.isEmpty(columns)) {
            throw new DBException("ScratchBird draft object requires at least one column on " + qualifiedName(tableBase));
        }
        return columns.get(0);
    }

    @NotNull
    static GenericUniqueKey defaultReferenceConstraint(
        @NotNull org.jkiss.dbeaver.model.runtime.DBRProgressMonitor monitor,
        @NotNull GenericTableBase tableBase
    ) throws DBException {
        List<GenericUniqueKey> constraints = tableBase.getConstraints(monitor);
        if (!CommonUtils.isEmpty(constraints)) {
            for (GenericUniqueKey constraint : constraints) {
                if (!constraint.getConstraintType().isUnique()) {
                    continue;
                }
                if (!CommonUtils.isEmpty(constraint.getAttributeReferences(monitor))) {
                    return constraint;
                }
            }
        }

        GenericTableColumn firstColumn = firstTableColumn(monitor, tableBase);
        GenericUniqueKey syntheticConstraint = new GenericUniqueKey(
            tableBase,
            "pk_" + tableBase.getName(),
            null,
            DBSEntityConstraintType.PRIMARY_KEY,
            true);
        syntheticConstraint.addColumn(new GenericTableConstraintColumn(syntheticConstraint, firstColumn, 1));
        return syntheticConstraint;
    }

    @NotNull
    static GenericTableColumn firstConstraintColumn(
        @NotNull org.jkiss.dbeaver.model.runtime.DBRProgressMonitor monitor,
        @NotNull GenericUniqueKey constraint
    ) throws DBException {
        List<? extends GenericTableConstraintColumn> columns = constraint.getAttributeReferences(monitor);
        if (CommonUtils.isEmpty(columns) || columns.get(0).getAttribute() == null) {
            return firstTableColumn(monitor, constraint.getTable());
        }
        return columns.get(0).getAttribute();
    }

    @NotNull
    static String containerPath(@NotNull GenericStructContainer container) {
        if (container instanceof ScratchBirdSchemaNode schemaNode) {
            return schemaNode.getAuthorityPath();
        }
        if (container.getSchema() != null) {
            return container.getSchema().getName();
        }
        return container.getName();
    }

    @Nullable
    static String objectPath(@NotNull DBSObject object) {
        if (object instanceof ScratchBirdTable scratchBirdTable) {
            return scratchBirdTable.getAuthorityPath();
        }
        if (object instanceof ScratchBirdView scratchBirdView) {
            return scratchBirdView.getAuthorityPath();
        }
        if (object instanceof GenericTableBase tableBase) {
            return containerPath(tableBase.getContainer());
        }
        if (object.getParentObject() instanceof GenericTableBase tableBase) {
            return containerPath(tableBase.getContainer());
        }
        if (object.getParentObject() instanceof GenericStructContainer structContainer) {
            return containerPath(structContainer);
        }
        return null;
    }

    @NotNull
    static String qualifiedName(@NotNull DBSObject object) {
        if (object instanceof DBPQualifiedObject qualifiedObject) {
            return qualifiedObject.getFullyQualifiedName(DBPEvaluationContext.DDL);
        }
        if (object.getParentObject() instanceof GenericStructContainer structContainer) {
            return DBUtils.getFullQualifiedName(
                object.getDataSource(),
                structContainer.getCatalog(),
                structContainer.getSchema(),
                object);
        }
        return DBUtils.getQuotedIdentifier(object);
    }

    @NotNull
    static String defaultProcedureSource(@NotNull GenericProcedure procedure) {
        return "CREATE PROCEDURE " + qualifiedName(procedure) + "()\n" +
            "AS\n" +
            "BEGIN\n" +
            "END";
    }

    @NotNull
    static String defaultTableTriggerSource(@NotNull GenericTableTrigger trigger) {
        return "CREATE TRIGGER " + qualifiedName(trigger) + "\n" +
            "BEFORE INSERT ON " + qualifiedName(trigger.getTable()) + "\n" +
            "AS\n" +
            "BEGIN\n" +
            "END";
    }
}
