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
package org.jkiss.dbeaver.ext.scratchbird.ui;

import org.eclipse.jface.dialogs.TitleAreaDialog;
import org.eclipse.jface.dialogs.IDialogConstants;
import org.eclipse.swt.SWT;
import org.eclipse.swt.dnd.Clipboard;
import org.eclipse.swt.dnd.TextTransfer;
import org.eclipse.swt.dnd.Transfer;
import org.eclipse.swt.layout.GridData;
import org.eclipse.swt.layout.GridLayout;
import org.eclipse.swt.widgets.Button;
import org.eclipse.swt.widgets.Combo;
import org.eclipse.swt.widgets.Composite;
import org.eclipse.swt.widgets.Control;
import org.eclipse.swt.widgets.Label;
import org.eclipse.swt.widgets.Shell;
import org.eclipse.swt.widgets.TabFolder;
import org.eclipse.swt.widgets.TabItem;
import org.eclipse.swt.widgets.Text;
import org.jkiss.code.NotNull;
import org.jkiss.code.Nullable;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdAdminExecutor;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdDestructivePlan;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdEditorPageCatalog;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdFormDefinition;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdFormPanelCatalog;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdFormMode;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdFormRegistry;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdLiveProbe;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdNavigatorActionRegistry;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdObjectFormContext;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdPermissionProbe;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdProbeHistory;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdRefusalModel;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdReportCatalog;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdReportDefinition;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdReportPlan;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdSchemaNode;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdTaskCatalog;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdTaskDefinition;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdValidationBridge;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdValueBinding;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdValueProfile;
import org.jkiss.dbeaver.model.struct.DBSObject;
import org.jkiss.dbeaver.model.struct.DBSTypedObject;
import org.jkiss.dbeaver.ui.UIUtils;

import java.lang.reflect.InvocationTargetException;
import java.util.Collection;
import java.util.List;

public class ScratchBirdManagementDialog extends TitleAreaDialog {

    private static final int COPY_SCRIPT_ID = IDialogConstants.CLIENT_ID + 1;
    private static final int COPY_REVIEW_PACKET_ID = IDialogConstants.CLIENT_ID + 2;
    private static final int RUN_LIVE_PROBE_ID = IDialogConstants.CLIENT_ID + 3;
    private static final int RUN_AUTHZ_PROBE_ID = IDialogConstants.CLIENT_ID + 4;

    @NotNull
    private final DBSObject targetObject;
    @NotNull
    private final ScratchBirdNavigatorActionRegistry.Action action;
    @NotNull
    private final ScratchBirdFormMode mode;
    @NotNull
    private final ScratchBirdFormDefinition form;
    @NotNull
    private final String targetPath;
    @NotNull
    private final String probeScopeKey;
    @Nullable
    private final ScratchBirdReportDefinition report;
    @NotNull
    private final ScratchBirdRefusalModel permission;
    @NotNull
    private final ScratchBirdLiveProbe.ProbePlan authzProbePlan;
    @NotNull
    private final ScratchBirdEditorPageCatalog.EditorPlan editorPlan;
    @NotNull
    private final ScratchBirdAdminExecutor.ExecutionPlan plan;
    @NotNull
    private final ScratchBirdLiveProbe.ProbePlan probePlan;
    @NotNull
    private final List<ScratchBirdTaskDefinition> taskDefinitions;
    @Nullable
    private final ScratchBirdDestructivePlan destructivePlan;
    @Nullable
    private ScratchBirdLiveProbe.ProbeResult authzProbeResult;
    @Nullable
    private ScratchBirdLiveProbe.ProbeResult liveProbeResult;
    @Nullable
    private ScratchBirdLiveProbe.ProbeResult taskProbeResult;
    @Nullable
    private ScratchBirdLiveProbe.TaskProbePhase taskProbePhase;
    private int selectedTaskIndex;
    @Nullable
    private Combo taskSelector;
    @Nullable
    private Button taskPreviewButton;
    @Nullable
    private Button taskValidateButton;
    @Nullable
    private Button taskExecuteButton;
    @Nullable
    private Text taskStatusText;
    @Nullable
    private Text taskSummaryText;
    @Nullable
    private Text taskModeText;
    @Nullable
    private Text taskSurfaceText;
    @Nullable
    private Text taskPreviewCommandText;
    @Nullable
    private Text taskValidateCommandText;
    @Nullable
    private Text taskExecuteCommandText;
    @Nullable
    private Text taskResultText;
    private int selectedHistoryIndex;
    @Nullable
    private Combo historySelector;
    @Nullable
    private Button clearHistoryButton;
    @Nullable
    private Text historyStatusText;
    @Nullable
    private Text historySummaryText;
    @Nullable
    private Text historyCommandText;
    @Nullable
    private Text historyOutputText;
    @Nullable
    private Text authzStatusText;
    @Nullable
    private Text authzSummaryText;
    @Nullable
    private Text authzCommandText;
    @Nullable
    private Text authzResultText;
    @Nullable
    private Text liveStatusText;
    @Nullable
    private Text liveSummaryText;
    @Nullable
    private Text liveCommandText;
    @Nullable
    private Text liveResultText;

    public ScratchBirdManagementDialog(
        @Nullable Shell parentShell,
        @NotNull DBSObject targetObject,
        @NotNull ScratchBirdNavigatorActionRegistry.Action action
    ) {
        super(parentShell);
        this.targetObject = targetObject;
        this.action = action;
        this.mode = modeFor(action);
        this.form = ScratchBirdFormRegistry.resolveForObject(targetObject, action);
        this.targetPath = selectedPath(targetObject);
        this.probeScopeKey = ScratchBirdProbeHistory.scopeKey(targetObject, targetPath);
        this.report = targetObject instanceof ScratchBirdSchemaNode schemaNode
            ? ScratchBirdReportCatalog.findByNavigatorPath(schemaNode.getFullPath())
            : null;
        this.permission = ScratchBirdPermissionProbe.probe(form, mode, targetPath);
        this.plan = ScratchBirdAdminExecutor.plan(
            form,
            mode,
            targetPath);
        this.editorPlan = ScratchBirdEditorPageCatalog.planFor(form, mode, targetPath);
        this.taskDefinitions = ScratchBirdTaskCatalog.tasksFor(targetPath);
        this.destructivePlan = mode == ScratchBirdFormMode.DELETE ?
            ScratchBirdDestructivePlan.forTarget(targetPath, plan.commandText()) :
            null;
        this.authzProbePlan = ScratchBirdPermissionProbe.planServerAuthorization(
            form,
            mode,
            targetPath,
            plan,
            taskDefinitions,
            destructivePlan);
        this.probePlan = ScratchBirdLiveProbe.plan(form, mode, targetPath, plan, taskDefinitions, destructivePlan);
    }

    @Override
    public void create() {
        super.create();
        setTitle(form.id() + " - " + form.name());
        setMessage("ScratchBird " + mode + " form for " + ScratchBirdSelectionUtils.displayPath(targetObject));
    }

    @Override
    protected Control createDialogArea(Composite parent) {
        Composite area = (Composite) super.createDialogArea(parent);

        TabFolder tabs = new TabFolder(area, SWT.TOP);
        tabs.setLayoutData(new GridData(SWT.FILL, SWT.FILL, true, true));

        createOverviewTab(tabs, permission);
        createEditorPages(tabs);
        createScratchBirdPanels(tabs);
        createAuthzTab(tabs);
        createObjectContextTab(tabs);
        createFieldMatrixTab(tabs);
        if (targetObject instanceof DBSTypedObject typedObject) {
            createValueTab(tabs, typedObject);
        }
        createExecutionTab(tabs, plan);
        if (destructivePlan != null) {
            createDestructiveTab(tabs, destructivePlan);
        }
        if (mode == ScratchBirdFormMode.TASK || "SBDV-FRM-016".equals(form.id())) {
            createTaskTab(tabs);
        }
        createLiveTab(tabs);
        createHistoryTab(tabs);
        createValidationTab(tabs, plan);
        if (report != null || action == ScratchBirdNavigatorActionRegistry.Action.REPORTS ||
            action == ScratchBirdNavigatorActionRegistry.Action.SOURCE_STATUS) {
            createReportTab(tabs);
        }

        return area;
    }

    @Override
    protected void createButtonsForButtonBar(Composite parent) {
        Button copyScript = createButton(parent, COPY_SCRIPT_ID, "Copy Script", false);
        copyScript.setToolTipText("Copy the generated ScratchBird SQL/admin preview.");
        Button copyPacket = createButton(parent, COPY_REVIEW_PACKET_ID, "Copy Form Packet", false);
        copyPacket.setToolTipText("Copy a review packet for this form, target, capability, and generated preview.");
        Button runAuthzProbe = createButton(parent, RUN_AUTHZ_PROBE_ID, "Run Authz Probe", false);
        runAuthzProbe.setToolTipText("Execute the safe server-backed authorization probe for this form.");
        runAuthzProbe.setEnabled(authzProbePlan.executable());
        Button runLiveProbe = createButton(parent, RUN_LIVE_PROBE_ID, "Run Live Probe", false);
        runLiveProbe.setToolTipText("Execute the safe live ScratchBird server probe for this form.");
        runLiveProbe.setEnabled(probePlan.executable());
        createButton(parent, IDialogConstants.OK_ID, IDialogConstants.OK_LABEL, true);
    }

    @Override
    protected void buttonPressed(int buttonId) {
        if (buttonId == COPY_SCRIPT_ID) {
            copyToClipboard(plan.commandText());
            setMessage("Generated ScratchBird preview copied to clipboard.");
            return;
        }
        if (buttonId == COPY_REVIEW_PACKET_ID) {
            copyToClipboard(reviewPacket());
            setMessage("ScratchBird form review packet copied to clipboard.");
            return;
        }
        if (buttonId == RUN_AUTHZ_PROBE_ID) {
            runAuthzProbe();
            return;
        }
        if (buttonId == RUN_LIVE_PROBE_ID) {
            runLiveProbe();
            return;
        }
        super.buttonPressed(buttonId);
    }

    private void createOverviewTab(
        @NotNull TabFolder tabs,
        @NotNull ScratchBirdRefusalModel permission
    ) {
        Composite container = createTab(tabs, "Overview");
        addField(container, "Action", action.name());
        addField(container, "Mode", mode.name());
        addField(container, "Target", targetPath);
        addField(container, "Object type", targetObject.getClass().getSimpleName());
        addField(container, "Form", form.summary());
        addField(container, "Scope", form.scope());
        addField(container, "Purpose", form.purpose());
        addField(container, "Capability", permission.kind() + ": " + permission.message());
        addField(container, "Server authz probe", authzProbePlan.label());
        addField(container, "Server authz ready", Boolean.toString(authzProbePlan.executable()));
        addField(container, "Task suggestions", Integer.toString(taskDefinitions.size()));
        addField(container, "Live probe", probePlan.label());
        addField(container, "Live probe ready", Boolean.toString(probePlan.executable()));
        if (targetObject instanceof DBSTypedObject typedObject) {
            ScratchBirdValueProfile valueProfile = ScratchBirdValueProfile.fromTypedObject(typedObject);
            addField(container, "Value profile", valueProfile.familyLabel() + " via " + valueProfile.handlerRouteLabel());
        }
    }

    private void createFieldMatrixTab(@NotNull TabFolder tabs) {
        Composite container = createTab(tabs, "Fields");
        addList(container, "Supported modes", form.modes().stream().map(ScratchBirdFormMode::name).toList());
        addList(container, "Must", form.mustFields());
        addList(container, "Should", form.shouldFields());
        addList(container, "Optional", form.optionalFields());
        addList(container, "Child forms", childFormSummaries(form.childForms()));
    }

    private void createEditorPages(@NotNull TabFolder tabs) {
        for (ScratchBirdEditorPageCatalog.EditorPage page : editorPlan.pages()) {
            Composite container = createTab(tabs, page.tabLabel());
            addField(container, "Page", page.title());
            addField(container, "Purpose", page.purpose());
            addList(container, "Controls", page.controls());
            addList(container, "Validation widgets", page.validationWidgets());
            addList(container, "Evidence anchors", page.evidenceAnchors());
        }
    }

    private void createObjectContextTab(@NotNull TabFolder tabs) {
        Composite container = createTab(tabs, "Object");
        for (ScratchBirdObjectFormContext.Field field : ScratchBirdObjectFormContext.fieldsFor(targetObject)) {
            addField(container, field.label(), field.value());
        }
    }

    private void createScratchBirdPanels(@NotNull TabFolder tabs) {
        DBSTypedObject typedObject = targetObject instanceof DBSTypedObject valueObject ? valueObject : null;
        for (ScratchBirdFormPanelCatalog.Panel panel : ScratchBirdFormPanelCatalog.panelsFor(
            form,
            mode,
            targetPath,
            permission,
            plan,
            authzProbePlan,
            probePlan,
            taskDefinitions,
            destructivePlan,
            typedObject)) {
            Composite container = createTab(tabs, panel.tabLabel());
            addField(container, "Panel", panel.title());
            for (ScratchBirdFormPanelCatalog.Entry entry : panel.entries()) {
                addField(container, entry.label(), entry.value());
            }
        }
    }

    private void createExecutionTab(
        @NotNull TabFolder tabs,
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan plan
    ) {
        Composite container = createTab(tabs, "Execution");
        addField(container, "Authority", plan.authority());
        addField(container, "Executable", Boolean.toString(plan.executable()));
        addField(container, "Destructive", Boolean.toString(plan.destructive()));
        addField(container, "Preview", plan.commandText());
    }

    private void createValueTab(@NotNull TabFolder tabs, @NotNull DBSTypedObject typedObject) {
        ScratchBirdValueProfile valueProfile = ScratchBirdValueProfile.fromTypedObject(typedObject);
        Composite container = createTab(tabs, "Value");
        addField(container, "Declared type", valueProfile.declaredTypeName());
        addField(container, "Datatype family", valueProfile.familyLabel());
        addField(container, "Value handler route", valueProfile.handlerRouteLabel());
        addField(container, "Canonical text contract", valueProfile.canonicalTextForm());
        addField(container, "Text roundtrip", valueProfile.explicitTextRoundTrip() ?
            "CAST(text AS " + valueProfile.declaredTypeName() + ") is expected to round-trip." :
            "Direct scalar/text literal handling is used.");
        addField(container, "Content type", valueProfile.contentTypeOrDefault());
        addField(container, "Example literal", ScratchBirdValueBinding.exampleLiteralForType(valueProfile.declaredTypeName()));
        addField(container, "Value-manager form", "SBDV-FRM-613 - ScratchBird Value Manager And Literal Editor");
    }

    private void createAuthzTab(@NotNull TabFolder tabs) {
        Composite container = createTab(tabs, "Authz");
        addField(container, "Static posture", permission.kind() + ": " + permission.message());
        authzStatusText = addFieldControl(container, "Server authz status", authzStatusSummary());
        authzSummaryText = addFieldControl(container, "Server authz summary", String.join("\n", authzProbePlan.summaryLines()));
        authzCommandText = addFieldControl(container, "Server authz commands", authzProbePlan.commandText());
        authzResultText = addFieldControl(container, "Server authz result", authzResultSummary());
    }

    private void createDestructiveTab(
        @NotNull TabFolder tabs,
        @NotNull ScratchBirdDestructivePlan destructivePlan
    ) {
        Composite container = createTab(tabs, "Destructive");
        addField(container, "Confirmation phrase", destructivePlan.confirmationPhrase());
        addList(container, "Dependency preview", destructivePlan.dependencyPreview());
        addList(container, "Dry-run / validate-only", destructivePlan.dryRunCommands());
        addList(container, "Schedule guidance", destructivePlan.scheduleGuidance());
        addList(container, "Rollback guidance", destructivePlan.rollbackGuidance());
        addList(container, "Result surfaces", destructivePlan.resultSurfaces());
    }

    private void createTaskTab(@NotNull TabFolder tabs) {
        Composite container = createTab(tabs, "Tasks");
        addField(container, "Target scope", targetPath);
        addField(container, "Capability posture", permission.kind() + ": " + permission.message());
        addField(container, "Default execute preview", plan.commandText());
        if (taskDefinitions.isEmpty()) {
            addField(container, "Task catalog", "No predefined task catalog is available for this target.");
            addList(container, "Suggested tasks", taskSummaries(taskDefinitions));
            return;
        }

        Label selectorLabel = new Label(container, SWT.NONE);
        selectorLabel.setText("Selected task");
        selectorLabel.setLayoutData(new GridData(SWT.LEFT, SWT.TOP, false, false));

        taskSelector = new Combo(container, SWT.DROP_DOWN | SWT.READ_ONLY);
        taskSelector.setItems(taskSummaries(taskDefinitions).toArray(String[]::new));
        taskSelector.setLayoutData(new GridData(SWT.FILL, SWT.TOP, true, false));
        taskSelector.select(Math.min(selectedTaskIndex, taskDefinitions.size() - 1));
        taskSelector.addListener(SWT.Selection, event -> {
            selectedTaskIndex = Math.max(0, taskSelector.getSelectionIndex());
            taskProbePhase = null;
            taskProbeResult = null;
            setErrorMessage(null);
            refreshTaskFields();
            ScratchBirdTaskDefinition activeTask = activeTask();
            if (activeTask != null) {
                setMessage("ScratchBird task context: " + activeTask.id() + " - " + activeTask.title());
            }
        });

        Composite buttonRow = new Composite(container, SWT.NONE);
        buttonRow.setLayoutData(new GridData(SWT.FILL, SWT.TOP, true, false, 2, 1));
        buttonRow.setLayout(new GridLayout(3, false));

        taskPreviewButton = new Button(buttonRow, SWT.PUSH);
        taskPreviewButton.setText("Run Task Preview");
        taskPreviewButton.addListener(SWT.Selection, event -> runTaskProbe(ScratchBirdLiveProbe.TaskProbePhase.PREVIEW));

        taskValidateButton = new Button(buttonRow, SWT.PUSH);
        taskValidateButton.setText("Run Task Validate");
        taskValidateButton.addListener(SWT.Selection, event -> runTaskProbe(ScratchBirdLiveProbe.TaskProbePhase.VALIDATE));

        taskExecuteButton = new Button(buttonRow, SWT.PUSH);
        taskExecuteButton.setText("Run Task Execute");
        taskExecuteButton.addListener(SWT.Selection, event -> runTaskProbe(ScratchBirdLiveProbe.TaskProbePhase.EXECUTE));

        taskStatusText = addFieldControl(container, "Task status", taskStatusSummary());
        taskSummaryText = addFieldControl(container, "Task summary", taskSummaryText());
        taskModeText = addFieldControl(container, "Execution modes", taskModeSummary());
        taskSurfaceText = addFieldControl(container, "Result surfaces", taskSurfaceSummary());
        taskPreviewCommandText = addFieldControl(container, "Preview command", taskCommandSummary(ScratchBirdLiveProbe.TaskProbePhase.PREVIEW));
        taskValidateCommandText = addFieldControl(container, "Validate command", taskCommandSummary(ScratchBirdLiveProbe.TaskProbePhase.VALIDATE));
        taskExecuteCommandText = addFieldControl(container, "Execute command", taskCommandSummary(ScratchBirdLiveProbe.TaskProbePhase.EXECUTE));
        taskResultText = addFieldControl(container, "Task result", taskResultSummary());
        refreshTaskFields();
    }

    private void createLiveTab(@NotNull TabFolder tabs) {
        Composite container = createTab(tabs, "Live");
        liveStatusText = addFieldControl(container, "Probe status", liveStatusSummary());
        liveSummaryText = addFieldControl(container, "Probe summary", String.join("\n", probePlan.summaryLines()));
        liveCommandText = addFieldControl(container, "Probe commands", probePlan.commandText());
        liveResultText = addFieldControl(container, "Probe result", liveResultSummary());
    }

    private void createHistoryTab(@NotNull TabFolder tabs) {
        Composite container = createTab(tabs, "History");
        addField(container, "Scope", targetPath);
        addField(container, "Scope key", probeScopeKey);
        addField(container, "Store location", ScratchBirdProbeHistory.storeLocationText());

        Label selectorLabel = new Label(container, SWT.NONE);
        selectorLabel.setText("Selected record");
        selectorLabel.setLayoutData(new GridData(SWT.LEFT, SWT.TOP, false, false));

        historySelector = new Combo(container, SWT.DROP_DOWN | SWT.READ_ONLY);
        historySelector.setLayoutData(new GridData(SWT.FILL, SWT.TOP, true, false));
        historySelector.addListener(SWT.Selection, event -> {
            selectedHistoryIndex = Math.max(0, historySelector.getSelectionIndex());
            refreshHistoryFields();
        });

        Composite buttonRow = new Composite(container, SWT.NONE);
        buttonRow.setLayoutData(new GridData(SWT.FILL, SWT.TOP, true, false, 2, 1));
        buttonRow.setLayout(new GridLayout(1, false));

        clearHistoryButton = new Button(buttonRow, SWT.PUSH);
        clearHistoryButton.setText("Clear Local History");
        clearHistoryButton.addListener(SWT.Selection, event -> {
            ScratchBirdProbeHistory.clear(probeScopeKey);
            selectedHistoryIndex = 0;
            refreshHistoryFields();
            setMessage("ScratchBird local probe/task history cleared for " + targetPath + ".");
        });

        historyStatusText = addFieldControl(container, "History status", historyStatusSummary());
        historySummaryText = addFieldControl(container, "Selected summary", historySummaryValue());
        historyCommandText = addFieldControl(container, "Recorded commands", historyCommandSummary());
        historyOutputText = addFieldControl(container, "Recorded output", historyOutputSummary());
        refreshHistoryFields();
    }

    private void createValidationTab(
        @NotNull TabFolder tabs,
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan plan
    ) {
        Composite container = createTab(tabs, "Validation");
        addList(container, "Statement inventory", ScratchBirdValidationBridge.statementSummaryFor(plan.commandText()));
        addList(container, "Parser diagnostics", ScratchBirdValidationBridge.diagnosticsFor(plan.commandText()));
        addList(container, "Lint hints", ScratchBirdValidationBridge.lintHintsFor(plan.commandText(), plan.commandText().length(), targetPath));
        addList(container, "Context hints", ScratchBirdValidationBridge.contextHintsFor(plan.commandText(), plan.commandText().length(), targetPath));
        addList(container, "Server probe hints", ScratchBirdValidationBridge.serverProbeHintsFor(plan.commandText(), plan.commandText().length(), targetPath));
        addList(container, "Form hints", ScratchBirdValidationBridge.formHintsFor(plan.commandText(), plan.commandText().length(), targetPath));
        addList(container, "Parser hints", ScratchBirdValidationBridge.completionHintsFor(plan.commandText(), plan.commandText().length()));
        addField(container, "Validation boundary", "Java v3 parser diagnostics are advisory; execution and permissions remain server-authoritative.");
    }

    private void createReportTab(@NotNull TabFolder tabs) {
        Composite container = createTab(tabs, "Reports");
        if (report != null) {
            addReportPlanFields(container, ScratchBirdReportPlan.forReport(report));
            return;
        }
        addList(container, "Available reports", reportSummaries(ScratchBirdReportCatalog.reportsForNavigatorPath(targetPath)));
    }

    private static void addReportPlanFields(
        @NotNull Composite container,
        @NotNull ScratchBirdReportPlan reportPlan
    ) {
        ScratchBirdRefusalModel sourceStatus = reportPlan.sourceStatus();
        ScratchBirdReportDefinition report = reportPlan.report();
        addField(container, "Report", report.id() + " - " + report.title());
        addField(container, "Branch", report.branch());
        addField(container, "Report output", report.bestOutput());
        addField(container, "Aggregation grain", report.aggregationGrain());
        addField(container, "Default retention", report.defaultRetention());
        addField(container, "Alert starter", report.alertStarter());
        addField(container, "Access notes", report.accessNotes());
        addField(container, "Source status", sourceStatus.kind() + ": " + sourceStatus.message());
        addField(container, "Future gated", Boolean.toString(report.futureGated()));
        addField(container, "Sources", String.join("\n", report.sourceSurfaces()));
        addField(container, "Source previews", String.join("\n", reportPlan.sourceQueries()));
        addList(container, "Drilldown fields", reportPlan.drilldownFields());
        addField(container, "Alert expression", reportPlan.alertExpressionStarter());
    }

    @NotNull
    private static Composite createTab(@NotNull TabFolder tabs, @NotNull String label) {
        TabItem item = new TabItem(tabs, SWT.NONE);
        item.setText(label);

        Composite container = new Composite(tabs, SWT.NONE);
        container.setLayout(new GridLayout(2, false));
        item.setControl(container);
        return container;
    }

    private static void addField(@NotNull Composite parent, @NotNull String label, @NotNull String value) {
        addFieldControl(parent, label, value);
    }

    @NotNull
    private static Text addFieldControl(@NotNull Composite parent, @NotNull String label, @NotNull String value) {
        Label labelControl = new Label(parent, SWT.NONE);
        labelControl.setText(label);
        labelControl.setLayoutData(new GridData(SWT.LEFT, SWT.TOP, false, false));

        Text text = new Text(parent, SWT.BORDER | SWT.READ_ONLY | SWT.WRAP | SWT.V_SCROLL);
        text.setText(value);
        GridData data = new GridData(SWT.FILL, SWT.TOP, true, false);
        data.heightHint = Math.min(90, Math.max(28, value.lines().count() > 1 ? 72 : 28));
        text.setLayoutData(data);
        return text;
    }

    private static void addList(@NotNull Composite parent, @NotNull String label, @NotNull List<String> values) {
        addField(parent, label, values.isEmpty() ? "-" : String.join("\n", values));
    }

    private void copyToClipboard(@NotNull String text) {
        Clipboard clipboard = new Clipboard(getShell().getDisplay());
        try {
            clipboard.setContents(new Object[]{text}, new Transfer[]{TextTransfer.getInstance()});
        } finally {
            clipboard.dispose();
        }
    }

    @NotNull
    private String reviewPacket() {
        StringBuilder packet = new StringBuilder();
        packet.append("ScratchBird DBeaver Form Review Packet\n");
        appendLine(packet, "Form", form.summary());
        appendLine(packet, "Action", action.name());
        appendLine(packet, "Mode", mode.name());
        appendLine(packet, "Target", targetPath);
        appendLine(packet, "Capability", permission.kind() + ": " + permission.message());
        appendLine(packet, "Executable", Boolean.toString(plan.executable()));
        appendLine(packet, "Destructive", Boolean.toString(plan.destructive()));
        appendLine(packet, "Authority", plan.authority());
        appendSection(packet, "Live probe plan", probePlan.summaryLines());
        appendSection(packet, "Object context", objectContextLines());
        appendEditorPages(packet);
        appendFormPanels(packet);
        if (targetObject instanceof DBSTypedObject typedObject) {
            appendValuePlan(packet, ScratchBirdValueProfile.fromTypedObject(typedObject));
        }
        appendSection(packet, "Must", form.mustFields());
        appendSection(packet, "Should", form.shouldFields());
        appendSection(packet, "Optional", form.optionalFields());
        appendSection(packet, "Child forms", childFormSummaries(form.childForms()));
        appendSection(packet, "Statement inventory", ScratchBirdValidationBridge.statementSummaryFor(plan.commandText()));
        appendSection(packet, "Parser diagnostics", ScratchBirdValidationBridge.diagnosticsFor(plan.commandText()));
        appendSection(packet, "Lint hints", ScratchBirdValidationBridge.lintHintsFor(plan.commandText(), plan.commandText().length(), targetPath));
        appendSection(packet, "Context hints", ScratchBirdValidationBridge.contextHintsFor(plan.commandText(), plan.commandText().length(), targetPath));
        appendSection(packet, "Server probe hints", ScratchBirdValidationBridge.serverProbeHintsFor(plan.commandText(), plan.commandText().length(), targetPath));
        appendSection(packet, "Form hints", ScratchBirdValidationBridge.formHintsFor(plan.commandText(), plan.commandText().length(), targetPath));
        appendSection(packet, "Parser hints", ScratchBirdValidationBridge.completionHintsFor(plan.commandText(), plan.commandText().length()));
        appendLine(packet, "Generated preview", plan.commandText());
        appendSection(packet, "Server authz plan", authzProbePlan.summaryLines());
        if (authzProbeResult != null) {
            appendSection(packet, "Server authz result", authzProbeResult.summaryLines());
            appendLine(packet, "Server authz output", authzProbeResult.previewText());
        }
        if (liveProbeResult != null) {
            appendSection(packet, "Live probe result", liveProbeResult.summaryLines());
            appendLine(packet, "Live probe output", liveProbeResult.previewText());
        }
        if (destructivePlan != null) {
            appendDestructivePlan(packet, destructivePlan);
        }
        if (mode == ScratchBirdFormMode.TASK || "SBDV-FRM-016".equals(form.id())) {
            appendTaskPlan(packet, taskDefinitions);
            appendSelectedTaskProbe(packet);
        }
        if (report != null) {
            appendReportPlan(packet, ScratchBirdReportPlan.forReport(report));
        } else if (action == ScratchBirdNavigatorActionRegistry.Action.REPORTS ||
            action == ScratchBirdNavigatorActionRegistry.Action.SOURCE_STATUS) {
            appendSection(packet, "Available reports", reportSummaries(ScratchBirdReportCatalog.reportsForNavigatorPath(targetPath)));
        }
        appendProbeHistory(packet);
        return packet.toString();
    }

    private static void appendLine(
        @NotNull StringBuilder builder,
        @NotNull String label,
        @NotNull String value
    ) {
        builder.append(label).append(": ").append(value).append('\n');
    }

    private static void appendSection(
        @NotNull StringBuilder builder,
        @NotNull String label,
        @NotNull List<String> values
    ) {
        builder.append(label).append(":\n");
        if (values.isEmpty()) {
            builder.append("- -\n");
            return;
        }
        for (String value : values) {
            builder.append("- ").append(value).append('\n');
        }
    }

    @NotNull
    private static List<String> childFormSummaries(@NotNull List<String> formIds) {
        return formIds.stream()
            .map(formId -> {
                ScratchBirdFormDefinition child = ScratchBirdFormRegistry.find(formId);
                return child == null ? formId + " - unresolved child form" : child.summary();
            })
            .toList();
    }

    @NotNull
    private static List<String> reportSummaries(@NotNull Collection<ScratchBirdReportDefinition> reports) {
        return reports.stream()
            .map(report -> report.id() + " - " + report.title() + " [" + report.branch() + "]")
            .toList();
    }

    private static void appendReportPlan(
        @NotNull StringBuilder packet,
        @NotNull ScratchBirdReportPlan reportPlan
    ) {
        appendSection(packet, "Report plan", reportPlan.summaryLines());
        appendSection(packet, "Report sources", reportPlan.report().sourceSurfaces());
        appendSection(packet, "Report source previews", reportPlan.sourceQueries());
        appendSection(packet, "Report drilldown fields", reportPlan.drilldownFields());
        appendLine(packet, "Report alert expression", reportPlan.alertExpressionStarter());
    }

    private static void appendValuePlan(
        @NotNull StringBuilder packet,
        @NotNull ScratchBirdValueProfile valueProfile
    ) {
        appendSection(packet, "Value profile", List.of(
            "Datatype family: " + valueProfile.familyLabel(),
            "Value handler route: " + valueProfile.handlerRouteLabel(),
            "Canonical text contract: " + valueProfile.canonicalTextForm(),
            "Content type: " + valueProfile.contentTypeOrDefault(),
            "Example literal: " + ScratchBirdValueBinding.exampleLiteralForType(valueProfile.declaredTypeName()),
            "Value-manager form: SBDV-FRM-613 - ScratchBird Value Manager And Literal Editor"));
    }

    private static void appendDestructivePlan(
        @NotNull StringBuilder packet,
        @NotNull ScratchBirdDestructivePlan destructivePlan
    ) {
        appendSection(packet, "Destructive flow", destructivePlan.summaryLines());
        appendSection(packet, "Dependency preview", destructivePlan.dependencyPreview());
        appendSection(packet, "Dry-run / validate-only", destructivePlan.dryRunCommands());
        appendSection(packet, "Schedule guidance", destructivePlan.scheduleGuidance());
        appendSection(packet, "Rollback guidance", destructivePlan.rollbackGuidance());
    }

    private static void appendTaskPlan(
        @NotNull StringBuilder packet,
        @NotNull List<ScratchBirdTaskDefinition> taskDefinitions
    ) {
        appendSection(packet, "Suggested tasks", taskSummaries(taskDefinitions));
        for (ScratchBirdTaskDefinition taskDefinition : taskDefinitions) {
            appendSection(packet, taskDefinition.id() + " - " + taskDefinition.title(), taskLines(taskDefinition));
        }
    }

    private void appendSelectedTaskProbe(@NotNull StringBuilder packet) {
        ScratchBirdTaskDefinition activeTask = activeTask();
        if (activeTask == null) {
            return;
        }
        appendSection(packet, "Selected task", taskLines(activeTask));
        appendSection(packet, "Task preview probe", taskProbePlan(ScratchBirdLiveProbe.TaskProbePhase.PREVIEW).summaryLines());
        appendSection(packet, "Task validate probe", taskProbePlan(ScratchBirdLiveProbe.TaskProbePhase.VALIDATE).summaryLines());
        appendSection(packet, "Task execute probe", taskProbePlan(ScratchBirdLiveProbe.TaskProbePhase.EXECUTE).summaryLines());
        if (taskProbeResult != null) {
            appendSection(packet, "Task live result", taskProbeResult.summaryLines());
            appendLine(packet, "Task live output", taskProbeResult.previewText());
        }
    }

    private void appendProbeHistory(@NotNull StringBuilder packet) {
        List<ScratchBirdProbeHistory.HistoryEntry> historyEntries = historyEntries();
        appendLine(packet, "Probe history store", ScratchBirdProbeHistory.storeLocationText());
        appendSection(packet, "Probe history", historyEntries.stream()
            .map(ScratchBirdProbeHistory.HistoryEntry::displayLabel)
            .toList());
        ScratchBirdProbeHistory.HistoryEntry selectedHistory = selectedHistoryEntry();
        if (selectedHistory != null) {
            appendSection(packet, "Selected history entry", selectedHistory.summaryLines());
            appendLine(packet, "Selected history commands", selectedHistory.commandText());
            appendLine(packet, "Selected history output", selectedHistory.previewText());
        }
    }

    @NotNull
    private static List<String> taskSummaries(@NotNull List<ScratchBirdTaskDefinition> taskDefinitions) {
        return taskDefinitions.stream()
            .map(task -> task.id() + " - " + task.title())
            .toList();
    }

    @NotNull
    private static List<String> taskLines(@NotNull ScratchBirdTaskDefinition taskDefinition) {
        return List.of(
            "Summary: " + taskDefinition.summary(),
            "Admission: " + taskDefinition.admissionNote(),
            "Execution modes: " + String.join(", ", taskDefinition.executionModes()),
            "Parameter template: " + String.join(" | ", taskDefinition.parameterTemplate()),
            "Result surfaces: " + String.join(", ", taskDefinition.resultSurfaces()),
            "Commands: " + String.join(" | ", taskDefinition.commandMatrix()));
    }

    @NotNull
    private List<String> objectContextLines() {
        return ScratchBirdObjectFormContext.fieldsFor(targetObject).stream()
            .map(field -> field.label() + ": " + field.value())
            .toList();
    }

    private void appendFormPanels(@NotNull StringBuilder packet) {
        DBSTypedObject typedObject = targetObject instanceof DBSTypedObject valueObject ? valueObject : null;
        for (ScratchBirdFormPanelCatalog.Panel panel : ScratchBirdFormPanelCatalog.panelsFor(
            form,
            mode,
            targetPath,
            permission,
            plan,
            authzProbePlan,
            probePlan,
            taskDefinitions,
            destructivePlan,
            typedObject)) {
            appendLine(packet, panel.tabLabel() + " panel", panel.title());
            appendSection(packet, panel.tabLabel() + " details", panel.entries().stream()
                .map(entry -> entry.label() + ": " + entry.value())
                .toList());
        }
    }

    private void appendEditorPages(@NotNull StringBuilder packet) {
        appendSection(packet, "Object-specific editor pages", editorPlan.summaryLines());
        for (ScratchBirdEditorPageCatalog.EditorPage page : editorPlan.pages()) {
            appendLine(packet, page.tabLabel() + " editor page", page.title());
            appendSection(packet, page.tabLabel() + " controls", page.controls());
            appendSection(packet, page.tabLabel() + " validation widgets", page.validationWidgets());
            appendSection(packet, page.tabLabel() + " evidence anchors", page.evidenceAnchors());
        }
    }

    @Nullable
    private ScratchBirdTaskDefinition activeTask() {
        if (taskDefinitions.isEmpty()) {
            return null;
        }
        selectedTaskIndex = Math.max(0, Math.min(selectedTaskIndex, taskDefinitions.size() - 1));
        return taskDefinitions.get(selectedTaskIndex);
    }

    @NotNull
    private List<ScratchBirdProbeHistory.HistoryEntry> historyEntries() {
        return ScratchBirdProbeHistory.historyFor(probeScopeKey);
    }

    @Nullable
    private ScratchBirdProbeHistory.HistoryEntry selectedHistoryEntry() {
        List<ScratchBirdProbeHistory.HistoryEntry> entries = historyEntries();
        if (entries.isEmpty()) {
            return null;
        }
        selectedHistoryIndex = Math.max(0, Math.min(selectedHistoryIndex, entries.size() - 1));
        return entries.get(selectedHistoryIndex);
    }

    @NotNull
    private ScratchBirdLiveProbe.ProbePlan taskProbePlan(@NotNull ScratchBirdLiveProbe.TaskProbePhase phase) {
        ScratchBirdTaskDefinition activeTask = activeTask();
        if (activeTask == null) {
            return new ScratchBirdLiveProbe.ProbePlan(
                "Task probe unavailable",
                "No predefined task catalog is available for this target.",
                plan.authority(),
                false,
                false,
                List.of());
        }
        return ScratchBirdLiveProbe.planForTask(activeTask, phase, plan.authority());
    }

    private void runTaskProbe(@NotNull ScratchBirdLiveProbe.TaskProbePhase phase) {
        ScratchBirdTaskDefinition activeTask = activeTask();
        if (activeTask == null) {
            setErrorMessage("No ScratchBird task is available for this target.");
            return;
        }
        ScratchBirdLiveProbe.ProbePlan taskPlan = taskProbePlan(phase);
        if (!taskPlan.executable()) {
            setErrorMessage("No safe live ScratchBird task " + phase.label().toLowerCase() + " probe is available for " + activeTask.id() + ".");
            taskProbePhase = phase;
            taskProbeResult = null;
            refreshTaskFields();
            return;
        }
        final ScratchBirdLiveProbe.ProbeResult[] resultHolder = new ScratchBirdLiveProbe.ProbeResult[1];
        try {
            UIUtils.runInProgressService(monitor -> resultHolder[0] = ScratchBirdLiveProbe.execute(monitor, targetObject, taskPlan));
        } catch (InvocationTargetException e) {
            setErrorMessage("Live ScratchBird task probe failed: " + e.getTargetException().getMessage());
            return;
        } catch (InterruptedException e) {
            setMessage("Live ScratchBird task probe canceled.");
            return;
        }
        taskProbePhase = phase;
        taskProbeResult = resultHolder[0];
        ScratchBirdProbeHistory.recordTaskProbe(probeScopeKey, targetPath, form, activeTask, phase, taskProbeResult);
        selectedHistoryIndex = 0;
        setErrorMessage(null);
        refreshTaskFields();
        refreshHistoryFields();
        setMessage(taskStatusSummary());
    }

    private void runAuthzProbe() {
        if (!authzProbePlan.executable()) {
            setErrorMessage("No safe server-backed authz probe is available for this form.");
            return;
        }
        final ScratchBirdLiveProbe.ProbeResult[] resultHolder = new ScratchBirdLiveProbe.ProbeResult[1];
        try {
            UIUtils.runInProgressService(monitor -> resultHolder[0] = ScratchBirdLiveProbe.execute(monitor, targetObject, authzProbePlan));
        } catch (InvocationTargetException e) {
            setErrorMessage("ScratchBird server authz probe failed: " + e.getTargetException().getMessage());
            return;
        } catch (InterruptedException e) {
            setMessage("ScratchBird server authz probe canceled.");
            return;
        }
        authzProbeResult = resultHolder[0];
        ScratchBirdProbeHistory.recordAuthorizationProbe(probeScopeKey, targetPath, form, authzProbeResult);
        selectedHistoryIndex = 0;
        setErrorMessage(null);
        refreshAuthzProbeFields();
        refreshHistoryFields();
        setMessage(authzStatusSummary());
    }

    private void runLiveProbe() {
        if (!probePlan.executable()) {
            setErrorMessage("No safe live ScratchBird server probe is available for this form.");
            return;
        }
        final ScratchBirdLiveProbe.ProbeResult[] resultHolder = new ScratchBirdLiveProbe.ProbeResult[1];
        try {
            UIUtils.runInProgressService(monitor -> resultHolder[0] = ScratchBirdLiveProbe.execute(monitor, targetObject, probePlan));
        } catch (InvocationTargetException e) {
            setErrorMessage("Live ScratchBird probe failed: " + e.getTargetException().getMessage());
            return;
        } catch (InterruptedException e) {
            setMessage("Live ScratchBird probe canceled.");
            return;
        }
        liveProbeResult = resultHolder[0];
        ScratchBirdProbeHistory.recordLiveProbe(probeScopeKey, targetPath, form, liveProbeResult);
        selectedHistoryIndex = 0;
        setErrorMessage(null);
        refreshLiveProbeFields();
        refreshHistoryFields();
        setMessage(liveStatusSummary());
    }

    private void refreshAuthzProbeFields() {
        if (authzStatusText != null && !authzStatusText.isDisposed()) {
            authzStatusText.setText(authzStatusSummary());
        }
        if (authzSummaryText != null && !authzSummaryText.isDisposed()) {
            authzSummaryText.setText(String.join("\n", authzProbePlan.summaryLines()));
        }
        if (authzCommandText != null && !authzCommandText.isDisposed()) {
            authzCommandText.setText(authzProbePlan.commandText());
        }
        if (authzResultText != null && !authzResultText.isDisposed()) {
            authzResultText.setText(authzResultSummary());
        }
    }

    private void refreshTaskFields() {
        if (taskStatusText != null && !taskStatusText.isDisposed()) {
            taskStatusText.setText(taskStatusSummary());
        }
        if (taskSummaryText != null && !taskSummaryText.isDisposed()) {
            taskSummaryText.setText(taskSummaryText());
        }
        if (taskModeText != null && !taskModeText.isDisposed()) {
            taskModeText.setText(taskModeSummary());
        }
        if (taskSurfaceText != null && !taskSurfaceText.isDisposed()) {
            taskSurfaceText.setText(taskSurfaceSummary());
        }
        if (taskPreviewCommandText != null && !taskPreviewCommandText.isDisposed()) {
            taskPreviewCommandText.setText(taskCommandSummary(ScratchBirdLiveProbe.TaskProbePhase.PREVIEW));
        }
        if (taskValidateCommandText != null && !taskValidateCommandText.isDisposed()) {
            taskValidateCommandText.setText(taskCommandSummary(ScratchBirdLiveProbe.TaskProbePhase.VALIDATE));
        }
        if (taskExecuteCommandText != null && !taskExecuteCommandText.isDisposed()) {
            taskExecuteCommandText.setText(taskCommandSummary(ScratchBirdLiveProbe.TaskProbePhase.EXECUTE));
        }
        if (taskResultText != null && !taskResultText.isDisposed()) {
            taskResultText.setText(taskResultSummary());
        }
        if (taskPreviewButton != null && !taskPreviewButton.isDisposed()) {
            taskPreviewButton.setEnabled(taskProbePlan(ScratchBirdLiveProbe.TaskProbePhase.PREVIEW).executable());
        }
        if (taskValidateButton != null && !taskValidateButton.isDisposed()) {
            taskValidateButton.setEnabled(taskProbePlan(ScratchBirdLiveProbe.TaskProbePhase.VALIDATE).executable());
        }
        if (taskExecuteButton != null && !taskExecuteButton.isDisposed()) {
            taskExecuteButton.setEnabled(taskProbePlan(ScratchBirdLiveProbe.TaskProbePhase.EXECUTE).executable());
        }
    }

    private void refreshLiveProbeFields() {
        if (liveStatusText != null && !liveStatusText.isDisposed()) {
            liveStatusText.setText(liveStatusSummary());
        }
        if (liveSummaryText != null && !liveSummaryText.isDisposed()) {
            liveSummaryText.setText(String.join("\n", probePlan.summaryLines()));
        }
        if (liveCommandText != null && !liveCommandText.isDisposed()) {
            liveCommandText.setText(probePlan.commandText());
        }
        if (liveResultText != null && !liveResultText.isDisposed()) {
            liveResultText.setText(liveResultSummary());
        }
    }

    private void refreshHistoryFields() {
        List<ScratchBirdProbeHistory.HistoryEntry> entries = historyEntries();
        if (historySelector != null && !historySelector.isDisposed()) {
            historySelector.setItems(entries.stream()
                .map(ScratchBirdProbeHistory.HistoryEntry::displayLabel)
                .toArray(String[]::new));
            if (entries.isEmpty()) {
                historySelector.deselectAll();
            } else {
                selectedHistoryIndex = Math.max(0, Math.min(selectedHistoryIndex, entries.size() - 1));
                historySelector.select(selectedHistoryIndex);
            }
        }
        if (clearHistoryButton != null && !clearHistoryButton.isDisposed()) {
            clearHistoryButton.setEnabled(!entries.isEmpty());
        }
        if (historyStatusText != null && !historyStatusText.isDisposed()) {
            historyStatusText.setText(historyStatusSummary());
        }
        if (historySummaryText != null && !historySummaryText.isDisposed()) {
            historySummaryText.setText(historySummaryValue());
        }
        if (historyCommandText != null && !historyCommandText.isDisposed()) {
            historyCommandText.setText(historyCommandSummary());
        }
        if (historyOutputText != null && !historyOutputText.isDisposed()) {
            historyOutputText.setText(historyOutputSummary());
        }
    }

    @NotNull
    private String authzStatusSummary() {
        if (authzProbeResult == null) {
            return authzProbePlan.executable() ?
                "Not yet executed. Use Run Authz Probe to verify server-backed capability inventory and branch authorization." :
                "No safe server-backed authz probe is available for this form.";
        }
        if (authzProbeResult.status().isAdmitted()) {
            return authzProbePlan.surrogate() ?
                "ADMITTED: Server-backed read-only authz probe completed; mutation capability remains unproven." :
                "ADMITTED: Server-backed authz probe completed successfully.";
        }
        return authzProbeResult.status().kind() + ": " + authzProbeResult.status().message();
    }

    @NotNull
    private String authzResultSummary() {
        return authzProbeResult == null ?
            "No server-backed authz result captured yet." :
            authzProbeResult.previewText();
    }

    @NotNull
    private String liveStatusSummary() {
        if (liveProbeResult == null) {
            return probePlan.executable() ?
                "Not yet executed. Use Run Live Probe to query the connected ScratchBird server." :
                "No safe live server probe is available for this form.";
        }
        return liveProbeResult.status().kind() + ": " + liveProbeResult.status().message();
    }

    @NotNull
    private String liveResultSummary() {
        return liveProbeResult == null ?
            "No live probe result captured yet." :
            liveProbeResult.previewText();
    }

    @NotNull
    private String historyStatusSummary() {
        List<ScratchBirdProbeHistory.HistoryEntry> entries = historyEntries();
        if (entries.isEmpty()) {
            return "No live or task history has been recorded yet for this ScratchBird scope. New entries are stored locally under the DBeaver metadata folder.";
        }
        return "Recorded entries: " + entries.size() + ". The newest result is selected by default and retained locally per ScratchBird scope across dialog reopen and application restart.";
    }

    @NotNull
    private String historySummaryValue() {
        ScratchBirdProbeHistory.HistoryEntry selectedHistory = selectedHistoryEntry();
        return selectedHistory == null ?
            "Run a live probe or a task probe to retain a local result log for this ScratchBird scope." :
            String.join("\n", selectedHistory.summaryLines());
    }

    @NotNull
    private String historyCommandSummary() {
        ScratchBirdProbeHistory.HistoryEntry selectedHistory = selectedHistoryEntry();
        return selectedHistory == null ?
            "No recorded commands yet." :
            selectedHistory.commandText();
    }

    @NotNull
    private String historyOutputSummary() {
        ScratchBirdProbeHistory.HistoryEntry selectedHistory = selectedHistoryEntry();
        return selectedHistory == null ?
            "No recorded output yet." :
            selectedHistory.previewText();
    }

    @NotNull
    private String taskStatusSummary() {
        ScratchBirdTaskDefinition activeTask = activeTask();
        if (activeTask == null) {
            return "No predefined ScratchBird task catalog is available for this target.";
        }
        if (taskProbeResult == null || taskProbePhase == null) {
            return "Not yet executed. Use the task probe controls to query the connected ScratchBird server for " + activeTask.id() + ".";
        }
        return taskProbePhase.label() + ": " + taskProbeResult.status().kind() + ": " + taskProbeResult.status().message();
    }

    @NotNull
    private String taskSummaryText() {
        ScratchBirdTaskDefinition activeTask = activeTask();
        return activeTask == null ?
            "No predefined ScratchBird task catalog is available for this target." :
            String.join("\n", taskLines(activeTask));
    }

    @NotNull
    private String taskModeSummary() {
        ScratchBirdTaskDefinition activeTask = activeTask();
        return activeTask == null ? "-" : String.join(", ", activeTask.executionModes());
    }

    @NotNull
    private String taskSurfaceSummary() {
        ScratchBirdTaskDefinition activeTask = activeTask();
        return activeTask == null ? "-" : String.join("\n", activeTask.resultSurfaces());
    }

    @NotNull
    private String taskCommandSummary(@NotNull ScratchBirdLiveProbe.TaskProbePhase phase) {
        return taskProbePlan(phase).commandText();
    }

    @NotNull
    private String taskResultSummary() {
        return taskProbeResult == null ?
            "No live task result captured yet." :
            taskProbeResult.previewText();
    }

    @NotNull
    private static String selectedPath(@NotNull DBSObject targetObject) {
        return targetObject instanceof ScratchBirdSchemaNode schemaNode ?
            schemaNode.getFullPath() :
            ScratchBirdSelectionUtils.displayPath(targetObject);
    }

    @NotNull
    private static ScratchBirdFormMode modeFor(@NotNull ScratchBirdNavigatorActionRegistry.Action action) {
        return switch (action) {
            case NEW -> ScratchBirdFormMode.CREATE;
            case ALTER -> ScratchBirdFormMode.ALTER;
            case DELETE -> ScratchBirdFormMode.DELETE;
            case TASKS -> ScratchBirdFormMode.TASK;
            case REPORTS, SOURCE_STATUS -> ScratchBirdFormMode.REPORT;
            case OPEN, PROPERTIES, REFRESH -> ScratchBirdFormMode.INSPECT;
        };
    }
}
