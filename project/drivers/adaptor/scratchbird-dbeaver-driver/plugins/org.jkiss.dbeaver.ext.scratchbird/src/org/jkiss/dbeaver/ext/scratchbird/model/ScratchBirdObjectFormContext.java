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
import org.jkiss.code.Nullable;
import org.jkiss.dbeaver.ext.generic.model.GenericDataType;
import org.jkiss.dbeaver.ext.generic.model.GenericTableBase;
import org.jkiss.dbeaver.model.DBPEvaluationContext;
import org.jkiss.dbeaver.model.DBPQualifiedObject;
import org.jkiss.dbeaver.model.struct.DBSEntityAttribute;
import org.jkiss.dbeaver.model.struct.DBSEntityConstraint;
import org.jkiss.dbeaver.model.struct.DBSObject;
import org.jkiss.dbeaver.model.struct.DBSTypedObject;
import org.jkiss.dbeaver.model.struct.rdb.DBSProcedure;
import org.jkiss.dbeaver.model.struct.rdb.DBSTableIndex;
import org.jkiss.dbeaver.model.struct.rdb.DBSTrigger;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public final class ScratchBirdObjectFormContext {

    public record Field(@NotNull String label, @NotNull String value) {
    }

    private static final List<String> NON_RELATIONAL_MARKERS = List.of(
        "document",
        "json",
        "key_value",
        "key-value",
        "key value",
        "kv",
        "graph",
        "vector",
        "search",
        "hybrid",
        "non_relational",
        "non-relational");

    private ScratchBirdObjectFormContext() {
    }

    @NotNull
    public static String displayPath(@NotNull DBSObject object) {
        if (object instanceof ScratchBirdSchemaNode schemaNode) {
            return schemaNode.getFullPath();
        }
        if (object instanceof DBPQualifiedObject qualifiedObject) {
            return qualifiedObject.getFullyQualifiedName(DBPEvaluationContext.DDL);
        }
        return object.getName();
    }

    @NotNull
    public static List<Field> fieldsFor(@NotNull DBSObject object) {
        List<Field> fields = new ArrayList<>();
        add(fields, "Display path", displayPath(object));
        add(fields, "Object name", object.getName());
        add(fields, "Object class", object.getClass().getName());
        add(fields, "Persisted", Boolean.toString(object.isPersisted()));
        add(fields, "Parent object", displayName(object.getParentObject()));
        add(fields, "Description", object.getDescription());

        addSchemaFields(fields, object);
        addQualifiedFields(fields, object);
        addTableFields(fields, object);
        addTypedFields(fields, object);
        addConstraintFields(fields, object);
        addRoutineFields(fields, object);
        addTriggerFields(fields, object);
        return List.copyOf(fields);
    }

    public static boolean isLikelyNonRelationalTable(@NotNull GenericTableBase table) {
        String type = normalize(table.getTableType());
        String name = normalize(table.getName());
        for (String marker : NON_RELATIONAL_MARKERS) {
            if (type.contains(marker) || name.contains(marker)) {
                return true;
            }
        }
        return false;
    }

    private static void addSchemaFields(@NotNull List<Field> fields, @NotNull DBSObject object) {
        if (object instanceof ScratchBirdSchemaNode schemaNode) {
            ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(schemaNode.getFullPath());
            add(fields, "ScratchBird full path", schemaNode.getFullPath());
            add(fields, "Database UUID", schemaNode.getDatabaseUuid());
            add(fields, "Object UUID", schemaNode.getObjectUuid());
            add(fields, "Parent UUID", schemaNode.getParentUuid());
            add(fields, "Object type", schemaNode.getObjectType());
            add(fields, "Identity status", schemaNode.getIdentityStatus());
            add(fields, "Catalog backed", Boolean.toString(schemaNode.isCatalogBacked()));
            add(fields, "Client-only branch", Boolean.toString(schemaNode.isClientOnly()));
            add(fields, "Domain branch", Boolean.toString(schemaNode.isDomainBranch()));
            add(fields, "Metrics branch", Boolean.toString(schemaNode.isMetricsBranch()));
            add(fields, "Branch profile", profile.id() + " - " + profile.label());
            add(fields, "Form route", profile.formId());
            add(fields, "Mutation posture", profile.mutationSummary());
            add(fields, "Expected child kinds", String.join(", ", profile.expectedChildren()));
            add(fields, "Focus fields", String.join(", ", profile.focusFields()));
        }
    }

    private static void addQualifiedFields(@NotNull List<Field> fields, @NotNull DBSObject object) {
        if (object instanceof DBPQualifiedObject qualifiedObject) {
            add(fields, "Qualified DDL name", qualifiedObject.getFullyQualifiedName(DBPEvaluationContext.DDL));
        }
    }

    private static void addTableFields(@NotNull List<Field> fields, @NotNull DBSObject object) {
        if (object instanceof GenericTableBase table) {
            add(fields, "Table/view kind", table.isView() ? "view" : "table");
            add(fields, "ScratchBird model family", isLikelyNonRelationalTable(table) ? "non-relational" : "relational");
            add(fields, "JDBC table type", table.getTableType());
            add(fields, "System object", Boolean.toString(table.isSystem()));
            add(fields, "Utility object", Boolean.toString(table.isUtility()));
            add(fields, "Catalog", displayName(table.getCatalog()));
            add(fields, "Schema", displayName(table.getSchema()));
        }
    }

    private static void addTypedFields(@NotNull List<Field> fields, @NotNull DBSObject object) {
        if (object instanceof DBSTypedObject typedObject) {
            ScratchBirdValueProfile valueProfile = ScratchBirdValueProfile.fromTypedObject(typedObject);
            add(fields, "Type name", typedObject.getTypeName());
            add(fields, "Full type name", typedObject.getFullTypeName());
            add(fields, "Data kind", typedObject.getDataKind().name());
            add(fields, "Precision", stringify(typedObject.getPrecision()));
            add(fields, "Scale", stringify(typedObject.getScale()));
            add(fields, "Max length", Long.toString(typedObject.getMaxLength()));
            add(fields, "ScratchBird datatype family", valueProfile.familyLabel());
            add(fields, "ScratchBird value handler", valueProfile.handlerRouteLabel());
            add(fields, "Canonical text contract", valueProfile.canonicalTextForm());
            add(fields, "Value MIME/content type", valueProfile.contentType());
            add(fields, "Value literal example", ScratchBirdValueBinding.exampleLiteralForType(valueProfile.declaredTypeName()));
        }
        if (object instanceof DBSEntityAttribute attribute) {
            add(fields, "Ordinal position", Integer.toString(attribute.getOrdinalPosition()));
            add(fields, "Required", Boolean.toString(attribute.isRequired()));
            add(fields, "Generated", Boolean.toString(attribute.isAutoGenerated()));
            add(fields, "Default value", attribute.getDefaultValue());
        }
        if (object instanceof GenericDataType) {
            add(fields, "ScratchBird domain/data type route", "sys.domains");
        }
    }

    private static void addConstraintFields(@NotNull List<Field> fields, @NotNull DBSObject object) {
        if (object instanceof DBSEntityConstraint constraint) {
            add(fields, "Constraint type", constraint.getConstraintType().getName());
            add(fields, "Constraint table/entity", displayName(constraint.getParentObject()));
        }
        if (object instanceof DBSTableIndex index) {
            add(fields, "Index type", index.getIndexType().getName());
            add(fields, "Unique index", Boolean.toString(index.isUnique()));
            add(fields, "Primary index", Boolean.toString(index.isPrimary()));
            add(fields, "Index table", displayName(index.getTable()));
            add(fields, "Index container", displayName(index.getContainer()));
        }
    }

    private static void addRoutineFields(@NotNull List<Field> fields, @NotNull DBSObject object) {
        if (object instanceof DBSProcedure procedure) {
            add(fields, "Procedure type", procedure.getProcedureType().name());
            add(fields, "Procedure container", displayName(procedure.getContainer()));
        }
    }

    private static void addTriggerFields(@NotNull List<Field> fields, @NotNull DBSObject object) {
        if (object instanceof DBSTrigger trigger) {
            add(fields, "Trigger table", displayName(trigger.getTable()));
        }
    }

    private static void add(
        @NotNull List<Field> fields,
        @NotNull String label,
        @Nullable String value
    ) {
        if (value == null || value.isBlank()) {
            return;
        }
        fields.add(new Field(label, value));
    }

    @NotNull
    private static String displayName(@Nullable DBSObject object) {
        return object == null ? "-" : displayPath(object);
    }

    @NotNull
    private static String stringify(@Nullable Object value) {
        return value == null ? "-" : value.toString();
    }

    @NotNull
    private static String normalize(@Nullable String value) {
        return value == null ? "" : value.toLowerCase(Locale.ROOT);
    }
}
