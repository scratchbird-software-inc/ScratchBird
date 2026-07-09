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
import org.eclipse.swt.accessibility.AccessibleAdapter;
import org.eclipse.swt.accessibility.AccessibleEvent;
import org.eclipse.swt.custom.SashForm;
import org.eclipse.swt.custom.ScrolledComposite;
import org.eclipse.swt.dnd.Clipboard;
import org.eclipse.swt.dnd.TextTransfer;
import org.eclipse.swt.dnd.Transfer;
import org.eclipse.swt.graphics.Point;
import org.eclipse.swt.graphics.Rectangle;
import org.eclipse.swt.layout.GridData;
import org.eclipse.swt.layout.GridLayout;
import org.eclipse.swt.widgets.Button;
import org.eclipse.swt.widgets.Combo;
import org.eclipse.swt.widgets.Composite;
import org.eclipse.swt.widgets.Control;
import org.eclipse.swt.widgets.Display;
import org.eclipse.swt.widgets.Label;
import org.eclipse.swt.widgets.Shell;
import org.eclipse.swt.widgets.TabFolder;
import org.eclipse.swt.widgets.TabItem;
import org.eclipse.swt.widgets.Table;
import org.eclipse.swt.widgets.TableColumn;
import org.eclipse.swt.widgets.TableItem;
import org.eclipse.swt.widgets.Text;
import org.jkiss.code.NotNull;
import org.jkiss.code.Nullable;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdAdminExecutor;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdDataEditorContract;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdDataTransferContract;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdDestructivePlan;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdEditorPageCatalog;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdFormDefinition;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdFormPanelCatalog;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdFormMode;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdFormRegistry;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdLiveProbe;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdManagementActionEnvelope;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdManagementConsoleCatalog;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdManagementSurfaceCatalog;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdManagementSurfaceDefinition;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdMutationApplyExecutor;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdNavigatorActionRegistry;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdObjectFormContext;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdObjectGraphContract;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdPermissionProbe;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdProbeHistory;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdRefusalModel;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdReportCatalog;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdReportDefinition;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdReportPlan;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdSchemaNode;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdSqlObjectEditorCatalog;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdTaskCatalog;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdTaskDefinition;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdValidationBridge;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdValueBinding;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdValueProfile;
import org.jkiss.dbeaver.model.struct.DBSObject;
import org.jkiss.dbeaver.model.struct.DBSTypedObject;
import org.jkiss.dbeaver.ui.UIUtils;

import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Locale;

public class ScratchBirdManagementDialog extends TitleAreaDialog {

    private static final int MIN_DIALOG_WIDTH = 1360;
    private static final int MIN_DIALOG_HEIGHT = 760;
    private static final int COPY_SCRIPT_ID = IDialogConstants.CLIENT_ID + 1;
    private static final int COPY_REVIEW_PACKET_ID = IDialogConstants.CLIENT_ID + 2;
    private static final int RUN_LIVE_PROBE_ID = IDialogConstants.CLIENT_ID + 3;
    private static final int RUN_AUTHZ_PROBE_ID = IDialogConstants.CLIENT_ID + 4;
    private static final int VALIDATE_PREVIEW_ID = IDialogConstants.CLIENT_ID + 5;
    private static final int REFRESH_SERVER_STATUS_ID = IDialogConstants.CLIENT_ID + 6;
    private static final int APPLY_REQUIRES_ADMISSION_ID = IDialogConstants.CLIENT_ID + 7;

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
    @Nullable
    private final ScratchBirdManagementSurfaceDefinition managementSurface;
    @NotNull
    private final ScratchBirdRefusalModel permission;
    @NotNull
    private final ScratchBirdLiveProbe.ProbePlan authzProbePlan;
    @NotNull
    private final ScratchBirdEditorPageCatalog.EditorPlan editorPlan;
    @NotNull
    private final ScratchBirdAdminExecutor.ExecutionPlan plan;
    @NotNull
    private final ScratchBirdManagementActionEnvelope actionEnvelope;
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
    private ScratchBirdMutationApplyExecutor.ApplyResult applyResult;
    @Nullable
    private ScratchBirdLiveProbe.TaskProbePhase taskProbePhase;
    @Nullable
    private Shell hostShell;
    private int selectedTaskIndex;
    @Nullable
    private Button applyButton;
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
    @Nullable
    private ScratchBirdManagementConsoleCatalog.ConsoleDefinition managementConsole;
    @Nullable
    private ScratchBirdSqlObjectEditorCatalog.EditorDefinition objectEditor;
    @Nullable
    private Text objectEditorDraftText;
    @Nullable
    private Text objectEditorCreateText;
    @Nullable
    private Text objectEditorAlterText;
    @Nullable
    private Text objectEditorDropText;
    @Nullable
    private Text objectEditorValidationText;
    @Nullable
    private Table managementConsoleTable;
    @Nullable
    private Text managementConsoleAgentNameText;
    @Nullable
    private Text managementConsoleAgentLayerText;
    @Nullable
    private Text managementConsoleAgentScopeText;
    @Nullable
    private Text managementConsoleAgentStateText;
    @Nullable
    private Text managementConsoleAgentHealthText;
    @Nullable
    private Text managementConsoleAgentEnabledText;
    @Nullable
    private Text managementConsoleAgentActivePolicyText;
    @Nullable
    private Text managementConsoleAgentLastActivityText;
    @Nullable
    private Text managementConsoleDetailText;
    @Nullable
    private Text managementConsoleRuntimeText;
    @Nullable
    private Text managementConsolePolicyText;
    @Nullable
    private Combo managementConsolePolicySelector;
    @Nullable
    private Text managementConsoleActionHistoryText;
    @Nullable
    private Text managementConsoleEvidenceText;
    @Nullable
    private Text managementConsoleActionsText;
    @Nullable
    private Text managementConsoleLiveText;
    @Nullable
    private Table managementConsoleRuntimeResultTable;
    @Nullable
    private Table managementConsoleMetricResultTable;
    @Nullable
    private Table managementConsolePolicyResultTable;
    @Nullable
    private Table managementConsoleOverrideResultTable;
    @Nullable
    private Table managementConsoleActionResultTable;
    @Nullable
    private Table managementConsoleActionAuditResultTable;
    @Nullable
    private Table managementConsoleEvidenceResultTable;
    @Nullable
    private Table managementConsoleEvidenceAuditResultTable;
    @Nullable
    private Text managementConsolePolicyNameText;
    @Nullable
    private Text managementConsolePolicyFamilyText;
    @Nullable
    private Text managementConsolePolicyVersionText;
    @Nullable
    private Text managementConsolePolicyStateText;
    @Nullable
    private Text managementConsolePolicyValidationText;
    @Nullable
    private Text managementConsolePolicyScheduleText;
    @Nullable
    private Text managementConsolePolicyBodyText;
    @Nullable
    private Text managementConsolePolicySourceStatusText;
    @Nullable
    private Text managementConsoleOverrideSourceStatusText;
    @NotNull
    private List<ManagementResultRow> managementConsolePolicyRows = List.of();
    @NotNull
    private String managementConsoleMutationCommand = "";
    @Nullable
    private Text workflowStatusText;
    @Nullable
    private Text workflowPreviewIdentityText;
    @Nullable
    private Text workflowValidationText;
    @Nullable
    private Text workflowApplyText;
    @Nullable
    private Text workflowRefreshText;
    @Nullable
    private Text workflowVerifyText;
    @Nullable
    private Text workflowRefusalText;
    @Nullable
    private Text workflowRollbackText;
    @Nullable
    private Text workflowLongOperationText;
    @Nullable
    private Text workflowAuditText;
    @Nullable
    private Text workflowFeatureBoundaryText;
    @NotNull
    private String localValidationSummary = "LOCAL_VALIDATION_PENDING: Preview has not been validated in this dialog session.";

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
        this.managementSurface = targetObject instanceof ScratchBirdSchemaNode schemaNode
            ? ScratchBirdManagementSurfaceCatalog.find(schemaNode.getFullPath())
            : null;
        this.permission = ScratchBirdPermissionProbe.probe(form, mode, targetPath);
        this.plan = ScratchBirdAdminExecutor.plan(
            form,
            mode,
            targetPath);
        this.actionEnvelope = ScratchBirdManagementActionEnvelope.forPlan(
            form,
            mode,
            targetPath,
            plan);
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
        this.probePlan = managementSurface != null ?
            ScratchBirdLiveProbe.planForManagementSurface(managementSurface, plan.authority()) :
            ScratchBirdLiveProbe.plan(form, mode, targetPath, plan, taskDefinitions, destructivePlan);
        this.managementConsole = managementSurface == null ? null : ScratchBirdManagementConsoleCatalog.forSurface(managementSurface);
        this.objectEditor = managementSurface == null ? ScratchBirdSqlObjectEditorCatalog.forForm(form, targetPath) : null;
    }

    @Override
    public void create() {
        super.create();
        hostShell = getShell();
        configureMinimumShellSize();
        if (managementConsole != null) {
            setTitle(managementConsole.title());
            setMessage("ScratchBird management console for " + targetPath);
            autoRefreshManagementConsole();
            return;
        }
        setTitle(form.id() + " - " + form.name());
        setMessage("ScratchBird " + mode + " form for " + ScratchBirdSelectionUtils.displayPath(targetObject));
    }

    @Override
    protected boolean isResizable() {
        return true;
    }

    @Override
    protected Point getInitialSize() {
        Shell shell = getShell();
        if (shell == null || shell.getDisplay() == null) {
            return new Point(1280, 820);
        }
        Rectangle clientArea = shell.getDisplay().getPrimaryMonitor().getClientArea();
        int width = Math.min(1500, Math.max(MIN_DIALOG_WIDTH, clientArea.width - 160));
        int height = Math.min(980, Math.max(MIN_DIALOG_HEIGHT, clientArea.height - 140));
        return new Point(width, height);
    }

    private void configureMinimumShellSize() {
        Shell shell = getShell();
        if (shell != null && !shell.isDisposed()) {
            shell.setMinimumSize(MIN_DIALOG_WIDTH, MIN_DIALOG_HEIGHT);
        }
    }

    @Override
    protected Control createDialogArea(Composite parent) {
        Composite area = (Composite) super.createDialogArea(parent);
        area.setLayoutData(new GridData(SWT.FILL, SWT.FILL, true, true));

        createFormTabs(area, true);
        return area;
    }

    public Control createEmbeddedEditorArea(@NotNull Composite parent) {
        hostShell = parent.getShell();
        Composite area = new Composite(parent, SWT.NONE);
        GridLayout layout = new GridLayout(1, false);
        layout.marginWidth = 0;
        layout.marginHeight = 0;
        area.setLayout(layout);
        area.setLayoutData(new GridData(SWT.FILL, SWT.FILL, true, true));

        Label title = new Label(area, SWT.NONE);
        title.setText(managementConsole == null ? form.id() + " - " + form.name() : managementConsole.title());
        title.setLayoutData(new GridData(SWT.FILL, SWT.TOP, true, false));
        setAccessibleName(title, "ScratchBird editor title");

        Label message = new Label(area, SWT.WRAP);
        message.setText(managementConsole == null ?
            "ScratchBird " + mode + " form for " + ScratchBirdSelectionUtils.displayPath(targetObject) :
            "ScratchBird management console for " + targetPath);
        message.setLayoutData(new GridData(SWT.FILL, SWT.TOP, true, false));
        setAccessibleName(message, "ScratchBird editor message");

        createFormTabs(area, false);
        createEmbeddedButtonBar(area);
        if (managementConsole != null) {
            autoRefreshManagementConsole();
        }
        return area;
    }

    private void createFormTabs(@NotNull Composite area, boolean scrollStandardForms) {
        if (managementSurface != null && managementConsole != null) {
            TabFolder tabs = createPlainTabFolder(area);
            createManagementConsoleTab(tabs, managementSurface, managementConsole);
            tabs.setSelection(0);
            return;
        }

        if (!scrollStandardForms) {
            TabFolder tabs = createPlainTabFolder(area);
            populateStandardTabs(tabs);
            return;
        }

        ScrolledComposite scroll = new ScrolledComposite(area, SWT.V_SCROLL | SWT.H_SCROLL);
        scroll.setExpandHorizontal(true);
        scroll.setExpandVertical(true);
        scroll.setAlwaysShowScrollBars(false);
        scroll.setLayoutData(new GridData(SWT.FILL, SWT.FILL, true, true));
        setAccessibleName(scroll, "ScratchBird management form scroll area");

        TabFolder tabs = new TabFolder(scroll, SWT.TOP);
        tabs.setLayoutData(new GridData(SWT.FILL, SWT.FILL, true, true));
        scroll.setContent(tabs);

        populateStandardTabs(tabs);
        configureDialogScroll(scroll, tabs);
    }

    @NotNull
    private static TabFolder createPlainTabFolder(@NotNull Composite area) {
        TabFolder tabs = new TabFolder(area, SWT.TOP);
        tabs.setLayoutData(new GridData(SWT.FILL, SWT.FILL, true, true));
        return tabs;
    }

    private void populateStandardTabs(@NotNull TabFolder tabs) {
        if (objectEditor != null) {
            createSqlObjectEditorTab(tabs, objectEditor);
        }
        createOverviewTab(tabs, permission);
        if (managementSurface != null) {
            createManagementSurfaceTab(tabs, managementSurface);
        }
        createWorkflowTab(tabs);
        createDataEditorContractTab(tabs);
        createDataTransferContractTab(tabs);
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
        createMonitoringTab(tabs);
        createObjectGraphTab(tabs);
        createHistoryTab(tabs);
        createValidationTab(tabs, plan);
        if (managementSurface != null || report != null || action == ScratchBirdNavigatorActionRegistry.Action.REPORTS ||
            action == ScratchBirdNavigatorActionRegistry.Action.SOURCE_STATUS) {
            createReportTab(tabs);
        }
        if ((managementConsole != null || objectEditor != null) && tabs.getItemCount() > 0) {
            tabs.setSelection(0);
        }
    }

    private static void configureDialogScroll(
        @NotNull ScrolledComposite scroll,
        @NotNull TabFolder tabs
    ) {
        Point preferred = tabs.computeSize(SWT.DEFAULT, SWT.DEFAULT);
        scroll.setMinSize(
            Math.max(1100, preferred.x),
            Math.max(760, preferred.y));
    }

    private void createEmbeddedButtonBar(@NotNull Composite parent) {
        Composite buttonRow = new Composite(parent, SWT.NONE);
        GridLayout layout = new GridLayout(7, true);
        layout.marginWidth = 8;
        layout.marginHeight = 8;
        layout.horizontalSpacing = 6;
        buttonRow.setLayout(layout);
        buttonRow.setLayoutData(new GridData(SWT.FILL, SWT.BOTTOM, true, false));

        createEmbeddedButton(buttonRow, "Copy Script", COPY_SCRIPT_ID,
            "Copy the generated ScratchBird SQL/admin preview.", "copy-preview-script");
        createEmbeddedButton(buttonRow, "Copy Form Packet", COPY_REVIEW_PACKET_ID,
            "Copy a review packet for this form, target, capability, and generated preview.", "copy-form-packet");
        createEmbeddedButton(buttonRow, "Validate Preview", VALIDATE_PREVIEW_ID,
            "Run the local ScratchBird parser and workflow proof for the generated preview.", "validate-preview");
        createEmbeddedButton(buttonRow, "Refresh Server Status", REFRESH_SERVER_STATUS_ID,
            "Run available server-backed authz and live refresh probes.", "refresh-server-status");
        createEmbeddedButton(buttonRow, "Run Authz Probe", RUN_AUTHZ_PROBE_ID,
            "Execute the safe server-backed authorization probe for this form.", "run-authz-probe");
        createEmbeddedButton(buttonRow, "Run Live Probe", RUN_LIVE_PROBE_ID,
            "Execute the safe live ScratchBird server probe for this form.", "run-live-probe");
        Button applyRequiresAdmission = createEmbeddedButton(
            buttonRow,
            ScratchBirdManagementWorkflow.applyButtonLabel(applyButtonReady()),
            APPLY_REQUIRES_ADMISSION_ID,
            "Run the mutation only after the server permission probe admits the exact preview and command hash.",
            "apply-requires-admission");
        applyButton = applyRequiresAdmission;
        applyRequiresAdmission.setEnabled(applyButtonReady());
    }

    @NotNull
    private Button createEmbeddedButton(
        @NotNull Composite parent,
        @NotNull String label,
        int buttonId,
        @NotNull String toolTip,
        @NotNull String proofId
    ) {
        Button button = new Button(parent, SWT.PUSH);
        button.setText(label);
        GridData data = new GridData(SWT.FILL, SWT.CENTER, true, false);
        data.minimumWidth = 118;
        button.setLayoutData(data);
        configureButton(button, toolTip, proofId);
        button.addListener(SWT.Selection, event -> buttonPressed(buttonId));
        return button;
    }

    @Override
    protected void createButtonsForButtonBar(Composite parent) {
        Button copyScript = createButton(parent, COPY_SCRIPT_ID, "Copy Script", false);
        configureButton(copyScript, "Copy the generated ScratchBird SQL/admin preview.", "copy-preview-script");
        Button copyPacket = createButton(parent, COPY_REVIEW_PACKET_ID, "Copy Form Packet", false);
        configureButton(copyPacket, "Copy a review packet for this form, target, capability, and generated preview.", "copy-form-packet");
        Button validatePreview = createButton(parent, VALIDATE_PREVIEW_ID, "Validate Preview", false);
        configureButton(validatePreview, "Run the local ScratchBird parser and workflow proof for the generated preview.", "validate-preview");
        Button refreshServerStatus = createButton(parent, REFRESH_SERVER_STATUS_ID, "Refresh Server Status", false);
        configureButton(refreshServerStatus, "Run available server-backed authz and live refresh probes.", "refresh-server-status");
        refreshServerStatus.setEnabled(activeAuthzProbePlan().executable() || probePlan.executable());
        Button runAuthzProbe = createButton(parent, RUN_AUTHZ_PROBE_ID, "Run Authz Probe", false);
        configureButton(runAuthzProbe, "Execute the safe server-backed authorization probe for this form.", "run-authz-probe");
        runAuthzProbe.setEnabled(activeAuthzProbePlan().executable());
        Button runLiveProbe = createButton(parent, RUN_LIVE_PROBE_ID, "Run Live Probe", false);
        configureButton(runLiveProbe, "Execute the safe live ScratchBird server probe for this form.", "run-live-probe");
        runLiveProbe.setEnabled(probePlan.executable());
        Button applyRequiresAdmission = createButton(
            parent,
            APPLY_REQUIRES_ADMISSION_ID,
            ScratchBirdManagementWorkflow.applyButtonLabel(applyButtonReady()),
            false);
        configureButton(
            applyRequiresAdmission,
            "Run the mutation only after the server permission probe admits the exact preview and command hash.",
            "apply-requires-admission");
        applyButton = applyRequiresAdmission;
        applyRequiresAdmission.setEnabled(applyButtonReady());
        createButton(parent, IDialogConstants.OK_ID, IDialogConstants.OK_LABEL, true);
    }

    @Override
    protected void buttonPressed(int buttonId) {
        if (buttonId == COPY_SCRIPT_ID) {
            copyToClipboard(currentCommandText());
            setMessage("Generated ScratchBird preview copied to clipboard.");
            return;
        }
        if (buttonId == COPY_REVIEW_PACKET_ID) {
            copyToClipboard(reviewPacket());
            setMessage("ScratchBird form review packet copied to clipboard.");
            return;
        }
        if (buttonId == VALIDATE_PREVIEW_ID) {
            validatePreview();
            return;
        }
        if (buttonId == REFRESH_SERVER_STATUS_ID) {
            refreshServerStatus();
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
        if (buttonId == APPLY_REQUIRES_ADMISSION_ID) {
            applyAfterAdmission();
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
        addField(container, "Feature boundary", actionEnvelope.featureBoundary().availability() + ": " + actionEnvelope.featureBoundary().uiState());
        addField(container, "Action envelope", actionEnvelope.envelopeId());
        addField(container, "Preview hash", currentPreviewHash());
        addField(container, "Server authz probe", activeAuthzProbePlan().label());
        addField(container, "Server authz ready", Boolean.toString(activeAuthzProbePlan().executable()));
        addField(container, "Task suggestions", Integer.toString(taskDefinitions.size()));
        addField(container, "Live probe", probePlan.label());
        addField(container, "Live probe ready", Boolean.toString(probePlan.executable()));
        if (targetObject instanceof DBSTypedObject typedObject) {
            ScratchBirdValueProfile valueProfile = ScratchBirdValueProfile.fromTypedObject(typedObject);
            addField(container, "Value profile", valueProfile.familyLabel() + " via " + valueProfile.handlerRouteLabel());
        }
    }

    private void createWorkflowTab(@NotNull TabFolder tabs) {
        Composite container = createTab(tabs, "Workflow");
        workflowStatusText = addFieldControl(container, "Workflow state", workflowStatusSummary());
        workflowPreviewIdentityText = addFieldControl(container, "Preview identity", ScratchBirdManagementWorkflow.previewIdentity(activeExecutionPlan()));
        workflowValidationText = addFieldControl(container, "Validate result", localValidationSummary);
        workflowApplyText = addFieldControl(container, "Apply gate", applyGateSummary());
        workflowRefreshText = addFieldControl(container, "Server refresh", refreshStatusSummary());
        workflowVerifyText = addFieldControl(container, "Verify result", verifyStatusSummary());
        workflowRefusalText = addFieldControl(container, "Refusal display", refusalSummary());
        workflowRollbackText = addFieldControl(container, "Rollback guidance", rollbackSummary());
        workflowLongOperationText = addFieldControl(container, "Long operation lifecycle", ScratchBirdManagementWorkflow.longOperationSummary());
        workflowAuditText = addFieldControl(container, "Audit visibility", auditSummary());
        workflowFeatureBoundaryText = addFieldControl(container, "Feature boundary", featureBoundarySummary());
        addList(container, "Workflow contract coverage", ScratchBirdManagementWorkflow.contractSummaryLines(targetPath));
        addList(container, "Accessibility and localization", ScratchBirdManagementWorkflow.accessibilityLocalizationLines());
    }

    private void createDataEditorContractTab(@NotNull TabFolder tabs) {
        Composite container = createTab(tabs, "Data Editor");
        addList(container, "Data editor workflow", ScratchBirdManagementWorkflow.dataEditorLines(targetPath));
        addField(container, "Apply boundary", applyGateSummary());
        addField(container, "Refresh boundary", refreshStatusSummary());
        for (ScratchBirdDataEditorContract.Operation operation : ScratchBirdDataEditorContract.Operation.values()) {
            ScratchBirdDataEditorContract.EditorPlan editorPlan = ScratchBirdDataEditorContract.plan(operation, targetPath);
            addField(container, operation.name() + " preview", editorPlan.previewCommand());
            addField(container, operation.name() + " admission", editorPlan.admissionProbeCommand());
            addList(container, operation.name() + " transaction proof", editorPlan.transactionProof());
            addList(container, operation.name() + " type proof", editorPlan.typeProof());
            addList(container, operation.name() + " refusal proof", editorPlan.refusalProof());
        }
    }

    private void createDataTransferContractTab(@NotNull TabFolder tabs) {
        Composite container = createTab(tabs, "Data Transfer");
        addList(container, "Data transfer workflow", ScratchBirdManagementWorkflow.dataTransferLines(targetPath));
        addField(container, "Long operation boundary", ScratchBirdManagementWorkflow.longOperationSummary());
        addField(container, "Feature boundary", featureBoundarySummary());
        for (ScratchBirdDataTransferContract.Direction direction : ScratchBirdDataTransferContract.Direction.values()) {
            ScratchBirdDataTransferContract.TransferPlan transferPlan = ScratchBirdDataTransferContract.plan(direction, targetPath);
            addField(container, direction.name() + " preview", transferPlan.previewCommand());
            addField(container, direction.name() + " authorization", transferPlan.authorizationProbe());
            addList(container, direction.name() + " batching rules", transferPlan.batchingRules());
            addList(container, direction.name() + " encoding rules", transferPlan.encodingRules());
            addList(container, direction.name() + " result proof", transferPlan.resultProof());
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
            activeExecutionPlan(),
            activeAuthzProbePlan(),
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
        addField(container, "Envelope id", actionEnvelope.envelopeId());
        addField(container, "Preview hash", currentPreviewHash());
        addField(container, "SBLR/UUID policy", actionEnvelope.sblrUuidPolicy());
        addField(container, "Transaction authority", actionEnvelope.transactionAuthority());
        addField(container, "Feature refusal", actionEnvelope.featureBoundary().refusalCode());
        addField(container, "Admission probe", activeAuthzProbePlan().commandText());
        addField(container, "Preview", currentCommandText());
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
        authzSummaryText = addFieldControl(container, "Server authz summary", String.join("\n", activeAuthzProbePlan().summaryLines()));
        authzCommandText = addFieldControl(container, "Server authz commands", activeAuthzProbePlan().commandText());
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
        addField(container, "Default execute preview", currentCommandText());
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
        taskSelector.setToolTipText("Selected ScratchBird task");
        setAccessibleName(taskSelector, "Selected ScratchBird task");
        taskSelector.setData(ScratchBirdManagementWorkflow.PROOF_DATA_KEY, "selected-task");
        taskSelector.select(Math.min(selectedTaskIndex, taskDefinitions.size() - 1));
        taskSelector.addListener(SWT.Selection, event -> {
            selectedTaskIndex = Math.max(0, taskSelector.getSelectionIndex());
            taskProbePhase = null;
            taskProbeResult = null;
            setErrorMessage(null);
            refreshTaskFields();
            refreshWorkflowFields();
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
        configureButton(taskPreviewButton, "Run the selected task preview probe when it is read-only and server-backed.", "task-preview");
        taskPreviewButton.addListener(SWT.Selection, event -> runTaskProbe(ScratchBirdLiveProbe.TaskProbePhase.PREVIEW));

        taskValidateButton = new Button(buttonRow, SWT.PUSH);
        taskValidateButton.setText("Run Task Validate");
        configureButton(taskValidateButton, "Run the selected task validate probe when it is read-only and server-backed.", "task-validate");
        taskValidateButton.addListener(SWT.Selection, event -> runTaskProbe(ScratchBirdLiveProbe.TaskProbePhase.VALIDATE));

        taskExecuteButton = new Button(buttonRow, SWT.PUSH);
        taskExecuteButton.setText("Run Task Execute");
        configureButton(taskExecuteButton, "Run the selected task execute probe only when the command remains read-only.", "task-execute");
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

    private void createSqlObjectEditorTab(
        @NotNull TabFolder tabs,
        @NotNull ScratchBirdSqlObjectEditorCatalog.EditorDefinition editor
    ) {
        Composite container = createTab(tabs, "Object Editor");
        addField(container, "Editor", editor.title());
        addField(container, "Object family", editor.objectFamily());
        addField(container, "Target", targetPath);
        addField(container, "Mode", mode.name());
        addList(container, "Primary fields", editor.primaryFields());
        addList(container, "Recursive schema rules", editor.recursiveSchemaRules());
        addList(container, "Security surfaces", editor.securitySurfaces());
        addList(container, "DDL capabilities", editor.ddlCapabilities());
        addList(container, "Server source queries", editor.sourceQueries());

        Label draftLabel = new Label(container, SWT.NONE);
        draftLabel.setText("Draft SBsql");
        draftLabel.setToolTipText("Editable ScratchBird SQL draft for this object lifecycle action");
        draftLabel.setLayoutData(new GridData(SWT.LEFT, SWT.TOP, false, false));
        setAccessibleName(draftLabel, "Draft ScratchBird SQL label");

        objectEditorDraftText = new Text(container, SWT.BORDER | SWT.MULTI | SWT.WRAP | SWT.V_SCROLL | SWT.H_SCROLL);
        objectEditorDraftText.setText(plan.commandText());
        objectEditorDraftText.setToolTipText("Edit the ScratchBird SQL draft, then validate and run the server authz probe before applying.");
        objectEditorDraftText.setData(ScratchBirdManagementWorkflow.PROOF_DATA_KEY, "sql-object-editor-draft-sbsql");
        setAccessibleName(objectEditorDraftText, "Editable ScratchBird SQL draft");
        GridData draftData = new GridData(SWT.FILL, SWT.FILL, true, true);
        draftData.heightHint = 160;
        objectEditorDraftText.setLayoutData(draftData);
        objectEditorDraftText.addListener(SWT.Modify, event -> {
            authzProbeResult = null;
            applyResult = null;
            localValidationSummary = "LOCAL_VALIDATION_PENDING: Draft SBsql changed and must be revalidated before server admission.";
            refreshObjectEditorFields();
            refreshAuthzProbeFields();
            refreshWorkflowFields();
            setMessage("Draft ScratchBird SQL changed; run Validate Preview and Run Authz Probe before applying.");
        });

        objectEditorCreateText = addFieldControl(container, "Create preview", lifecyclePreview(ScratchBirdFormMode.CREATE));
        objectEditorAlterText = addFieldControl(container, "Alter preview", lifecyclePreview(ScratchBirdFormMode.ALTER));
        objectEditorDropText = addFieldControl(container, "Drop preview", lifecyclePreview(ScratchBirdFormMode.DELETE));
        objectEditorValidationText = addFieldControl(container, "Editor validation", objectEditorValidationSummary(editor));
    }

    private void createManagementConsoleTab(
        @NotNull TabFolder tabs,
        @NotNull ScratchBirdManagementSurfaceDefinition surface,
        @NotNull ScratchBirdManagementConsoleCatalog.ConsoleDefinition console
    ) {
        TabItem item = new TabItem(tabs, SWT.NONE);
        item.setText(console.tabLabel());

        Composite container = new Composite(tabs, SWT.NONE);
        container.setLayout(new GridLayout(1, false));
        item.setControl(container);

        SashForm sash = new SashForm(container, SWT.VERTICAL);
        sash.setLayoutData(new GridData(SWT.FILL, SWT.FILL, true, true));

        Composite inventoryPanel = new Composite(sash, SWT.NONE);
        inventoryPanel.setLayout(new GridLayout(1, false));

        Label tableLabel = new Label(inventoryPanel, SWT.NONE);
        tableLabel.setText(console.itemLabel());
        tableLabel.setToolTipText(console.itemLabel());
        tableLabel.setLayoutData(new GridData(SWT.LEFT, SWT.TOP, false, false));
        setAccessibleName(tableLabel, console.itemLabel() + " label");

        managementConsoleTable = new Table(inventoryPanel, SWT.BORDER | SWT.FULL_SELECTION | SWT.SINGLE | SWT.V_SCROLL | SWT.H_SCROLL);
        managementConsoleTable.setHeaderVisible(true);
        managementConsoleTable.setLinesVisible(true);
        managementConsoleTable.setToolTipText(console.title());
        managementConsoleTable.setData(ScratchBirdManagementWorkflow.PROOF_DATA_KEY, "management-console-table");
        setAccessibleName(managementConsoleTable, console.title() + " table");
        GridData tableData = new GridData(SWT.FILL, SWT.FILL, true, true);
        tableData.heightHint = 220;
        tableData.minimumHeight = 120;
        managementConsoleTable.setLayoutData(tableData);

        addManagementConsoleColumn(managementConsoleTable, console.itemLabel(), 260);
        addManagementConsoleColumn(managementConsoleTable, "Kind", 150);
        addManagementConsoleColumn(managementConsoleTable, "State", 160);
        addManagementConsoleColumn(managementConsoleTable, "management.agents".equals(console.path()) ? "Scope" : "Server data", 420);

        for (ScratchBirdManagementConsoleCatalog.ConsoleRow row : console.rows()) {
            TableItem tableItem = new TableItem(managementConsoleTable, SWT.NONE);
            tableItem.setText(new String[]{row.label(), row.kind(), row.state(), row.source()});
            tableItem.setData(row);
            if (!row.enabled()) {
                tableItem.setForeground(container.getDisplay().getSystemColor(SWT.COLOR_DARK_GRAY));
            }
        }
        for (TableColumn column : managementConsoleTable.getColumns()) {
            column.pack();
        }
        managementConsoleTable.addListener(SWT.Selection, event -> {
            refreshManagementConsoleFields();
            ScratchBirdManagementConsoleCatalog.ConsoleRow row = selectedManagementConsoleRow();
            if (row != null) {
                setMessage(console.title() + ": " + row.label());
            }
        });
        if (managementConsoleTable.getItemCount() > 0) {
            managementConsoleTable.select(0);
        }

        TabFolder detailTabs = new TabFolder(sash, SWT.BORDER);
        detailTabs.setLayoutData(new GridData(SWT.FILL, SWT.FILL, true, true));
        createManagementConsoleDetailTabs(detailTabs, console);
        sash.setWeights(new int[]{34, 66});
    }

    private void createManagementConsoleDetailTabs(
        @NotNull TabFolder tabs,
        @NotNull ScratchBirdManagementConsoleCatalog.ConsoleDefinition console
    ) {
        Composite selected = createTab(tabs, console.itemLabel());
        managementConsoleAgentNameText = addFieldControl(selected, console.itemLabel(), "");
        managementConsoleAgentLayerText = addFieldControl(selected, "Kind", "");
        managementConsoleAgentScopeText = addFieldControl(selected, "Scope", "");
        managementConsoleAgentStateText = addFieldControl(selected, "State", "");
        managementConsoleAgentHealthText = addFieldControl(selected, "Health", "");
        managementConsoleAgentEnabledText = addFieldControl(selected, "Enabled", "");
        managementConsoleAgentActivePolicyText = addFieldControl(selected, "Active policy", "");
        managementConsoleAgentLastActivityText = addFieldControl(selected, "Last activity", "");
        managementConsoleDetailText = addTallFieldControl(selected, "Details", managementConsoleSelectedDetail());

        Composite runtime = createTab(tabs, "Runtime");
        managementConsoleRuntimeText = addTallFieldControl(runtime, "Runtime status", managementConsoleRuntimeSummary());
        managementConsoleRuntimeResultTable = addResultTable(runtime, "Runtime rows");
        managementConsoleMetricResultTable = addResultTable(runtime, "Metric dependencies");
        managementConsoleLiveText = addTallFieldControl(runtime, "Refresh result", managementConsoleLiveSummary());

        Composite policy = createTab(tabs, "Policy");
        Label selectorLabel = new Label(policy, SWT.NONE);
        selectorLabel.setText("Policy");
        selectorLabel.setToolTipText("Policy");
        selectorLabel.setLayoutData(new GridData(SWT.LEFT, SWT.TOP, false, false));
        setAccessibleName(selectorLabel, "Policy selector label");

        managementConsolePolicySelector = new Combo(policy, SWT.DROP_DOWN | SWT.READ_ONLY);
        managementConsolePolicySelector.setLayoutData(new GridData(SWT.FILL, SWT.TOP, true, false));
        managementConsolePolicySelector.setToolTipText("Visible policy document for the selected management item");
        managementConsolePolicySelector.setData(ScratchBirdManagementWorkflow.PROOF_DATA_KEY, "management-policy-document-selector");
        setAccessibleName(managementConsolePolicySelector, "Visible policy document selector");
        managementConsolePolicySelector.addListener(SWT.Selection, event -> refreshManagementConsolePolicyEditor());

        managementConsolePolicyText = addFieldControl(policy, "Policy summary", managementConsolePolicySummary());
        managementConsolePolicyNameText = addEditableFieldControl(policy, "Policy name", "");
        managementConsolePolicyFamilyText = addEditableFieldControl(policy, "Policy family", "");
        managementConsolePolicyVersionText = addEditableFieldControl(policy, "Version", "");
        managementConsolePolicyStateText = addFieldControl(policy, "Active state", "");
        managementConsolePolicyValidationText = addFieldControl(policy, "Validation state", "");
        managementConsolePolicyScheduleText = addEditableFieldControl(policy, "Schedule / override", "");
        managementConsolePolicyBodyText = addEditableTallFieldControl(policy, "Policy body / config", "");
        Composite policyButtons = new Composite(policy, SWT.NONE);
        policyButtons.setLayoutData(new GridData(SWT.FILL, SWT.TOP, true, false, 2, 1));
        policyButtons.setLayout(new GridLayout(5, false));
        Button newPolicy = new Button(policyButtons, SWT.PUSH);
        newPolicy.setText("New Draft");
        configureButton(newPolicy, "Start a new policy draft for the selected agent.", "agent-policy-new-draft");
        newPolicy.addListener(SWT.Selection, event -> newAgentPolicyDraft());
        Button validatePolicy = new Button(policyButtons, SWT.PUSH);
        validatePolicy.setText("Validate Policy");
        configureButton(validatePolicy, "Prepare a policy validation command for server admission.", "agent-policy-validate");
        validatePolicy.addListener(SWT.Selection, event -> prepareAgentPolicyCommand("VALIDATE"));
        Button simulatePolicy = new Button(policyButtons, SWT.PUSH);
        simulatePolicy.setText("Simulate Policy");
        configureButton(simulatePolicy, "Prepare a policy simulation command for server admission.", "agent-policy-simulate");
        simulatePolicy.addListener(SWT.Selection, event -> prepareAgentPolicyCommand("SIMULATE"));
        Button setActivePolicy = new Button(policyButtons, SWT.PUSH);
        setActivePolicy.setText("Set Active");
        configureButton(setActivePolicy, "Prepare a policy activation command for server admission.", "agent-policy-set-active");
        setActivePolicy.addListener(SWT.Selection, event -> prepareAgentPolicyCommand("ATTACH"));
        Button applyPolicy = new Button(policyButtons, SWT.PUSH);
        applyPolicy.setText("Apply Policy");
        configureButton(applyPolicy, "Prepare a policy apply command for server admission.", "agent-policy-apply");
        applyPolicy.addListener(SWT.Selection, event -> prepareAgentPolicyCommand("APPLY"));
        managementConsolePolicySourceStatusText = addTallFieldControl(policy, "Policy source", "Not refreshed. Use Refresh Server Status to load visible policy documents.");
        managementConsoleOverrideSourceStatusText = addTallFieldControl(policy, "Override source", "Not refreshed. Use Refresh Server Status to load visible schedule/override state.");

        Composite actions = createTab(tabs, "Actions");
        managementConsoleActionHistoryText = addTallFieldControl(actions, "Action history", managementConsoleActionHistorySummary());
        managementConsoleActionResultTable = addResultTable(actions, "Recent actions");
        managementConsoleActionAuditResultTable = addResultTable(actions, "Audit trail");
        managementConsoleActionsText = addTallFieldControl(actions, "Admissible actions", managementConsoleActionsSummary());

        Composite evidence = createTab(tabs, "Evidence");
        managementConsoleEvidenceText = addTallFieldControl(evidence, "Evidence and audit", managementConsoleEvidenceSummary());
        managementConsoleEvidenceResultTable = addResultTable(evidence, "Evidence rows");
        managementConsoleEvidenceAuditResultTable = addResultTable(evidence, "Audit rows");

        Composite notes = createTab(tabs, "Notes");
        addList(notes, "Operator notes", console.operatorNotes());
        refreshManagementConsoleResultTables();
    }

    private static void addManagementConsoleColumn(
        @NotNull Table table,
        @NotNull String label,
        int width
    ) {
        TableColumn column = new TableColumn(table, SWT.NONE);
        column.setText(label);
        column.setWidth(width);
    }

    private void createManagementSurfaceTab(
        @NotNull TabFolder tabs,
        @NotNull ScratchBirdManagementSurfaceDefinition surface
    ) {
        Composite container = createTab(tabs, "Surface");
        addField(container, "Surface path", surface.path());
        addField(container, "Display type", surface.displayType());
        addField(container, "Presentation", managementPresentation(surface));
        addField(container, "Required permission", surface.requiredPermission());
        addField(container, "Refresh policy", surface.refreshPolicy());
        addField(container, "Allowed actions", surface.actions().stream()
            .map(Enum::name)
            .reduce((a, b) -> a + ", " + b)
            .orElse("-"));
        addField(container, "Refusal behavior", surface.refusalBehavior());
        addField(container, "Data availability", managementDataAvailability(surface));
        addField(container, "Server truth boundary",
            "This is a ScratchBird management surface, not a physical schema. Values come from authorized sys.catalog_readable or SHOW sources; unavailable sources must display deterministic refusal text.");
    }

    private void createLiveTab(@NotNull TabFolder tabs) {
        Composite container = createTab(tabs, "Live");
        liveStatusText = addFieldControl(container, "Probe status", liveStatusSummary());
        liveSummaryText = addFieldControl(container, "Probe summary", String.join("\n", probePlan.summaryLines()));
        liveCommandText = addFieldControl(container, "Probe commands", probePlan.commandText());
        liveResultText = addFieldControl(container, "Probe result", liveResultSummary());
    }

    private void createMonitoringTab(@NotNull TabFolder tabs) {
        Composite container = createTab(tabs, "Monitoring");
        addList(container, "Dashboard surfaces", ScratchBirdManagementWorkflow.monitoringDashboardLines());
        addField(container, "Dashboard refresh status", refreshStatusSummary());
        addField(container, "Metric history status", historyStatusSummary());
        addField(container, "Dashboard refusal boundary",
            "Unsupported or unavailable sys views and SHOW surfaces remain visible refusals; no cached placeholder is presented as current.");
    }

    private void createObjectGraphTab(@NotNull TabFolder tabs) {
        ScratchBirdObjectGraphContract.GraphPlan graphPlan = ScratchBirdObjectGraphContract.plan(targetPath);
        Composite container = createTab(tabs, "Graph");
        addList(container, "Object graph workflow", ScratchBirdManagementWorkflow.objectGraphLines(targetPath));
        addField(container, "Dependency query", graphPlan.dependencyQuery());
        addField(container, "Search query", graphPlan.searchQuery());
        addField(container, "Generated DDL query", graphPlan.ddlPreviewQuery());
        addField(container, "Generated SBsql query", graphPlan.sbsqlPreviewQuery());
        addField(container, "Explain query", graphPlan.explainQuery());
        addList(container, "Visibility rules", graphPlan.visibilityRules());
        addField(container, "Search/DDL/SBsql status", verifyStatusSummary());
        addField(container, "Metadata invalidation", "Refresh from server truth is required before verify; stale or deleted targets are refused.");
        addField(container, "Authorization-filtered visibility", refusalSummary());
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
        historySelector.setToolTipText("Selected ScratchBird proof record");
        setAccessibleName(historySelector, "Selected ScratchBird proof record");
        historySelector.setData(ScratchBirdManagementWorkflow.PROOF_DATA_KEY, "selected-history-record");
        historySelector.addListener(SWT.Selection, event -> {
            selectedHistoryIndex = Math.max(0, historySelector.getSelectionIndex());
            refreshHistoryFields();
        });

        Composite buttonRow = new Composite(container, SWT.NONE);
        buttonRow.setLayoutData(new GridData(SWT.FILL, SWT.TOP, true, false, 2, 1));
        buttonRow.setLayout(new GridLayout(1, false));

        clearHistoryButton = new Button(buttonRow, SWT.PUSH);
        clearHistoryButton.setText("Clear Local History");
        configureButton(clearHistoryButton, "Clear local ScratchBird probe and task proof history for this scope.", "clear-local-history");
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
        addList(container, "Statement inventory", ScratchBirdValidationBridge.statementSummaryFor(currentCommandText()));
        addList(container, "Parser diagnostics", ScratchBirdValidationBridge.diagnosticsFor(currentCommandText()));
        addList(container, "Lint hints", ScratchBirdValidationBridge.lintHintsFor(currentCommandText(), currentCommandText().length(), targetPath));
        addList(container, "Context hints", ScratchBirdValidationBridge.contextHintsFor(currentCommandText(), currentCommandText().length(), targetPath));
        addList(container, "Server probe hints", ScratchBirdValidationBridge.serverProbeHintsFor(currentCommandText(), currentCommandText().length(), targetPath));
        addList(container, "Form hints", ScratchBirdValidationBridge.formHintsFor(currentCommandText(), currentCommandText().length(), targetPath));
        addList(container, "Parser hints", ScratchBirdValidationBridge.completionHintsFor(currentCommandText(), currentCommandText().length()));
        addField(container, "Validation boundary", "Java v3 parser diagnostics are advisory; execution and permissions remain server-authoritative.");
    }

    private void createReportTab(@NotNull TabFolder tabs) {
        Composite container = createTab(tabs, "Reports");
        if (report != null) {
            addReportPlanFields(container, ScratchBirdReportPlan.forReport(report));
            return;
        }
        addList(container, "Available reports", reportSummaries(ScratchBirdReportCatalog.reportsForNavigatorPath(targetPath)));
        if (managementSurface != null) {
            addField(container, "Management data availability", managementDataAvailability(managementSurface));
        }
    }

    @NotNull
    private static String managementPresentation(@NotNull ScratchBirdManagementSurfaceDefinition surface) {
        return switch (surface.displayType()) {
            case "dashboard" -> "Dashboard with status cards, result/refusal state, and refresh controls.";
            case "grid" -> "Authorization-filtered result grid sourced from server-published management data.";
            case "detail_form" -> "ScratchBird detail form with raw sys shortcut, admission proof, and redaction state.";
            case "action_form" -> "ScratchBird action form with preview, server admission, and guarded apply.";
            case "report" -> "Report surface with query/source preview, drilldown metadata, and alert starter.";
            default -> "ScratchBird management form.";
        };
    }

    @NotNull
    private static String managementDataAvailability(@NotNull ScratchBirdManagementSurfaceDefinition surface) {
        if (surface.backingSources().isEmpty()) {
            return "No live server data source is published for this surface yet.";
        }
        return surface.backingSources().size() + " server-published data surface(s), shown as result tables/status panes when the connected session is authorized.";
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
        labelControl.setToolTipText(label);
        labelControl.setLayoutData(new GridData(SWT.LEFT, SWT.TOP, false, false));
        setAccessibleName(labelControl, label + " label");

        Text text = new Text(parent, SWT.BORDER | SWT.READ_ONLY | SWT.WRAP | SWT.V_SCROLL);
        text.setText(value);
        text.setToolTipText(label);
        text.setData(ScratchBirdManagementWorkflow.PROOF_DATA_KEY, label);
        setAccessibleName(text, label + " value");
        GridData data = new GridData(SWT.FILL, SWT.TOP, true, false);
        data.heightHint = Math.min(90, Math.max(28, value.lines().count() > 1 ? 72 : 28));
        text.setLayoutData(data);
        return text;
    }

    @NotNull
    private static Text addTallFieldControl(@NotNull Composite parent, @NotNull String label, @NotNull String value) {
        Label labelControl = new Label(parent, SWT.NONE);
        labelControl.setText(label);
        labelControl.setToolTipText(label);
        labelControl.setLayoutData(new GridData(SWT.LEFT, SWT.TOP, false, false));
        setAccessibleName(labelControl, label + " label");

        Text text = new Text(parent, SWT.BORDER | SWT.READ_ONLY | SWT.MULTI | SWT.V_SCROLL | SWT.H_SCROLL);
        text.setText(value);
        text.setToolTipText(label);
        text.setData(ScratchBirdManagementWorkflow.PROOF_DATA_KEY, label);
        setAccessibleName(text, label + " value");
        GridData data = new GridData(SWT.FILL, SWT.FILL, true, true);
        data.heightHint = Math.min(210, Math.max(96, (int) Math.min(512, value.lines().count()) * 18 + 32));
        text.setLayoutData(data);
        return text;
    }

    @NotNull
    private static Table addResultTable(@NotNull Composite parent, @NotNull String label) {
        Label labelControl = new Label(parent, SWT.NONE);
        labelControl.setText(label);
        labelControl.setToolTipText(label);
        labelControl.setLayoutData(new GridData(SWT.LEFT, SWT.TOP, false, false));
        setAccessibleName(labelControl, label + " label");

        Table table = new Table(parent, SWT.BORDER | SWT.FULL_SELECTION | SWT.V_SCROLL | SWT.H_SCROLL);
        table.setHeaderVisible(true);
        table.setLinesVisible(true);
        table.setToolTipText(label);
        table.setData(ScratchBirdManagementWorkflow.PROOF_DATA_KEY, "management-console-result-table");
        setAccessibleName(table, label + " result table");
        GridData data = new GridData(SWT.FILL, SWT.FILL, true, true);
        data.heightHint = 150;
        table.setLayoutData(data);
        setStatusTable(table, "Not refreshed. Use Refresh Server Status to load server data.");
        return table;
    }

    @NotNull
    private static Text addEditableFieldControl(@NotNull Composite parent, @NotNull String label, @NotNull String value) {
        Label labelControl = new Label(parent, SWT.NONE);
        labelControl.setText(label);
        labelControl.setToolTipText(label);
        labelControl.setLayoutData(new GridData(SWT.LEFT, SWT.TOP, false, false));
        setAccessibleName(labelControl, label + " label");

        Text text = new Text(parent, SWT.BORDER);
        text.setText(value);
        text.setToolTipText(label);
        text.setData(ScratchBirdManagementWorkflow.PROOF_DATA_KEY, label);
        setAccessibleName(text, label + " value");
        text.setLayoutData(new GridData(SWT.FILL, SWT.TOP, true, false));
        return text;
    }

    @NotNull
    private static Text addEditableTallFieldControl(@NotNull Composite parent, @NotNull String label, @NotNull String value) {
        Label labelControl = new Label(parent, SWT.NONE);
        labelControl.setText(label);
        labelControl.setToolTipText(label);
        labelControl.setLayoutData(new GridData(SWT.LEFT, SWT.TOP, false, false));
        setAccessibleName(labelControl, label + " label");

        Text text = new Text(parent, SWT.BORDER | SWT.MULTI | SWT.WRAP | SWT.V_SCROLL | SWT.H_SCROLL);
        text.setText(value);
        text.setToolTipText(label);
        text.setData(ScratchBirdManagementWorkflow.PROOF_DATA_KEY, label);
        setAccessibleName(text, label + " value");
        GridData data = new GridData(SWT.FILL, SWT.FILL, true, true);
        data.heightHint = 120;
        text.setLayoutData(data);
        return text;
    }

    private static void setStatusTable(@Nullable Table table, @NotNull String message) {
        if (table == null || table.isDisposed()) {
            return;
        }
        resetTable(table, List.of("Status"));
        TableItem item = new TableItem(table, SWT.NONE);
        item.setText(new String[]{message});
        packTableColumns(table);
    }

    private void populateManagementResultTable(
        @Nullable Table table,
        @NotNull String label,
        @NotNull String source,
        @NotNull String filterColumn,
        @NotNull String filterValue
    ) {
        if (table == null || table.isDisposed()) {
            return;
        }
        if (source.isBlank()) {
            setStatusTable(table, label + " are not published for this management surface.");
            return;
        }
        if (liveProbeResult == null) {
            setStatusTable(table, label + " not refreshed. Use Refresh Server Status to load server data.");
            return;
        }
        if (liveProbeResult.status().isDeterministicRefusal()) {
            setStatusTable(table, liveProbeResult.status().kind() + ": " + liveProbeResult.status().redactedMessage());
            return;
        }

        ScratchBirdLiveProbe.StatementResult result = findManagementResult(source);
        if (result == null) {
            setStatusTable(table, "No server result was returned for " + readableSourceName(source) + ".");
            return;
        }
        if (!result.resultSet() || result.columns().isEmpty()) {
            setStatusTable(table, readableSourceName(source) + " returned no tabular result.");
            return;
        }

        List<List<String>> rows = filteredRows(result, filterColumn, filterValue);
        resetTable(table, result.columns());
        if (rows.isEmpty()) {
            TableItem item = new TableItem(table, SWT.NONE);
            item.setText(statusRow(result.columns().size(), "No rows are visible for the selected item."));
            packTableColumns(table);
            return;
        }
        for (List<String> row : rows) {
            TableItem item = new TableItem(table, SWT.NONE);
            item.setText(paddedRow(row, result.columns().size()));
            item.setData(new ManagementResultRow(result.columns(), row));
        }
        if (table.getItemCount() > 0) {
            table.select(0);
        }
        packTableColumns(table);
    }

    private record ManagementResultRow(
        @NotNull List<String> columns,
        @NotNull List<String> values
    ) {
        @NotNull
        String value(@NotNull String columnName) {
            int index = columnIndex(columns, columnName);
            return index >= 0 && index < values.size() ? values.get(index) : "";
        }
    }

    @Nullable
    private ScratchBirdLiveProbe.StatementResult findManagementResult(@NotNull String source) {
        if (liveProbeResult == null) {
            return null;
        }
        String normalizedSource = source.toLowerCase(Locale.ENGLISH);
        for (ScratchBirdLiveProbe.StatementResult result : liveProbeResult.statementResults()) {
            if (result.command().toLowerCase(Locale.ENGLISH).contains(normalizedSource)) {
                return result;
            }
        }
        return null;
    }

    @NotNull
    private static List<List<String>> filteredRows(
        @NotNull ScratchBirdLiveProbe.StatementResult result,
        @NotNull String filterColumn,
        @NotNull String filterValue
    ) {
        if (filterColumn.isBlank() || filterValue.isBlank()) {
            return result.sampleRows();
        }
        int filterIndex = columnIndex(result.columns(), filterColumn);
        if (filterIndex < 0) {
            return result.sampleRows();
        }
        List<List<String>> rows = new ArrayList<>();
        for (List<String> row : result.sampleRows()) {
            if (filterIndex < row.size() && filterValue.equalsIgnoreCase(row.get(filterIndex))) {
                rows.add(row);
            }
        }
        return List.copyOf(rows);
    }

    private static int columnIndex(@NotNull List<String> columns, @NotNull String expectedName) {
        for (int i = 0; i < columns.size(); i++) {
            if (expectedName.equalsIgnoreCase(columns.get(i))) {
                return i;
            }
        }
        return -1;
    }

    private static void resetTable(@NotNull Table table, @NotNull List<String> columns) {
        table.removeAll();
        for (TableColumn column : table.getColumns()) {
            column.dispose();
        }
        for (String columnName : columns) {
            TableColumn column = new TableColumn(table, SWT.NONE);
            column.setText(columnName);
            column.setWidth(160);
        }
    }

    private static void packTableColumns(@NotNull Table table) {
        for (TableColumn column : table.getColumns()) {
            column.pack();
        }
    }

    @NotNull
    private static String[] paddedRow(@NotNull List<String> row, int columnCount) {
        String[] values = new String[columnCount];
        for (int i = 0; i < columnCount; i++) {
            values[i] = i < row.size() ? row.get(i) : "";
        }
        return values;
    }

    @NotNull
    private static String[] statusRow(int columnCount, @NotNull String message) {
        String[] values = new String[Math.max(1, columnCount)];
        values[0] = message;
        for (int i = 1; i < values.length; i++) {
            values[i] = "";
        }
        return values;
    }

    @NotNull
    private static String readableSourceName(@NotNull String source) {
        int lastDot = source.lastIndexOf('.');
        return lastDot < 0 ? source : source.substring(lastDot + 1).replace('_', ' ');
    }

    private static void addList(@NotNull Composite parent, @NotNull String label, @NotNull List<String> values) {
        addField(parent, label, values.isEmpty() ? "-" : String.join("\n", values));
    }

    private static void configureButton(
        @NotNull Button button,
        @NotNull String toolTip,
        @NotNull String proofId
    ) {
        button.setToolTipText(toolTip);
        button.setData(ScratchBirdManagementWorkflow.PROOF_DATA_KEY, proofId);
        setAccessibleName(button, button.getText());
    }

    private static void setAccessibleName(@NotNull Control control, @NotNull String name) {
        control.setData(ScratchBirdManagementWorkflow.ACCESSIBLE_NAME_KEY, name);
        control.getAccessible().addAccessibleListener(new AccessibleAdapter() {
            @Override
            public void getName(AccessibleEvent event) {
                event.result = name;
            }
        });
    }

    private void copyToClipboard(@NotNull String text) {
        Clipboard clipboard = new Clipboard(activeDisplay());
        try {
            clipboard.setContents(new Object[]{text}, new Transfer[]{TextTransfer.getInstance()});
        } finally {
            clipboard.dispose();
        }
    }

    @NotNull
    private Display activeDisplay() {
        Shell shell = activeShell();
        return shell == null ? Display.getDefault() : shell.getDisplay();
    }

    @Nullable
    private Shell activeShell() {
        Shell shell = getShell();
        if (shell != null && !shell.isDisposed()) {
            return shell;
        }
        return hostShell == null || hostShell.isDisposed() ? null : hostShell;
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
        appendLine(packet, "Preview hash", currentPreviewHash());
        appendSection(packet, "Workflow status", List.of(
            workflowStatusSummary(),
            applyGateSummary(),
            refreshStatusSummary(),
            verifyStatusSummary(),
            refusalSummary()));
        appendSection(packet, "Action envelope", actionEnvelope.summaryLines());
        appendSection(packet, "Action review", actionEnvelope.reviewLines());
        appendSection(packet, "Session isolation", actionEnvelope.sessionScope().summaryLines());
        appendSection(packet, "Feature boundary", actionEnvelope.featureBoundary().summaryLines());
        appendSection(packet, "Workflow contract coverage", ScratchBirdManagementWorkflow.contractSummaryLines(targetPath));
        appendSection(packet, "Network policy", actionEnvelope.networkPolicy().summaryLines());
        appendSection(packet, "Data editor insert contract", ScratchBirdDataEditorContract.plan(ScratchBirdDataEditorContract.Operation.INSERT, targetPath).summaryLines());
        appendSection(packet, "Data editor update contract", ScratchBirdDataEditorContract.plan(ScratchBirdDataEditorContract.Operation.UPDATE, targetPath).summaryLines());
        appendSection(packet, "Data editor delete contract", ScratchBirdDataEditorContract.plan(ScratchBirdDataEditorContract.Operation.DELETE, targetPath).summaryLines());
        appendSection(packet, "Data editor refresh contract", ScratchBirdDataEditorContract.plan(ScratchBirdDataEditorContract.Operation.REFRESH, targetPath).summaryLines());
        appendSection(packet, "Data transfer import contract", ScratchBirdDataTransferContract.plan(ScratchBirdDataTransferContract.Direction.IMPORT, targetPath).summaryLines());
        appendSection(packet, "Data transfer export contract", ScratchBirdDataTransferContract.plan(ScratchBirdDataTransferContract.Direction.EXPORT, targetPath).summaryLines());
        appendSection(packet, "Object graph contract", ScratchBirdObjectGraphContract.plan(targetPath).summaryLines());
        appendSection(packet, "Monitoring dashboard surfaces", ScratchBirdManagementWorkflow.monitoringDashboardLines());
        appendSection(packet, "Object graph tooling boundary", ScratchBirdManagementWorkflow.objectGraphLines(targetPath));
        appendSection(packet, "Accessibility and localization", ScratchBirdManagementWorkflow.accessibilityLocalizationLines());
        appendSection(packet, "Manual QA proof anchors", ScratchBirdManagementWorkflow.manualQaChecklist());
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
        appendSection(packet, "Statement inventory", ScratchBirdValidationBridge.statementSummaryFor(currentCommandText()));
        appendSection(packet, "Parser diagnostics", ScratchBirdValidationBridge.diagnosticsFor(currentCommandText()));
        appendSection(packet, "Lint hints", ScratchBirdValidationBridge.lintHintsFor(currentCommandText(), currentCommandText().length(), targetPath));
        appendSection(packet, "Context hints", ScratchBirdValidationBridge.contextHintsFor(currentCommandText(), currentCommandText().length(), targetPath));
        appendSection(packet, "Server probe hints", ScratchBirdValidationBridge.serverProbeHintsFor(currentCommandText(), currentCommandText().length(), targetPath));
        appendSection(packet, "Form hints", ScratchBirdValidationBridge.formHintsFor(currentCommandText(), currentCommandText().length(), targetPath));
        appendSection(packet, "Parser hints", ScratchBirdValidationBridge.completionHintsFor(currentCommandText(), currentCommandText().length()));
        appendLine(packet, "Generated preview", currentCommandText());
        appendSection(packet, "Server authz plan", activeAuthzProbePlan().summaryLines());
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
            activeExecutionPlan(),
            activeAuthzProbePlan(),
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
        refreshWorkflowFields();
        refreshHistoryFields();
        setMessage(taskStatusSummary());
    }

    private void runAuthzProbe() {
        ScratchBirdLiveProbe.ProbePlan activeAuthzPlan = activeAuthzProbePlan();
        if (!activeAuthzPlan.executable()) {
            setErrorMessage("No safe server-backed authz probe is available for this form.");
            return;
        }
        final ScratchBirdLiveProbe.ProbeResult[] resultHolder = new ScratchBirdLiveProbe.ProbeResult[1];
        try {
            UIUtils.runInProgressService(monitor -> resultHolder[0] = ScratchBirdLiveProbe.execute(monitor, targetObject, activeAuthzPlan));
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
        refreshWorkflowFields();
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
        refreshWorkflowFields();
        refreshHistoryFields();
        setMessage(liveStatusSummary());
    }

    private void autoRefreshManagementConsole() {
        if (managementConsole == null || !probePlan.executable()) {
            return;
        }
        final ScratchBirdLiveProbe.ProbeResult[] resultHolder = new ScratchBirdLiveProbe.ProbeResult[1];
        try {
            UIUtils.runInProgressService(monitor -> resultHolder[0] = ScratchBirdLiveProbe.execute(monitor, targetObject, probePlan));
        } catch (InvocationTargetException e) {
            setErrorMessage("ScratchBird management data refresh failed: " + e.getTargetException().getMessage());
            return;
        } catch (InterruptedException e) {
            setMessage("ScratchBird management data refresh canceled.");
            return;
        }
        liveProbeResult = resultHolder[0];
        ScratchBirdProbeHistory.recordLiveProbe(probeScopeKey, targetPath, form, liveProbeResult);
        selectedHistoryIndex = 0;
        refreshLiveProbeFields();
        refreshWorkflowFields();
        refreshHistoryFields();
        setMessage("ScratchBird management data refreshed from the connected server.");
    }

    private void validatePreview() {
        localValidationSummary = ScratchBirdManagementWorkflow.validationSummary(
            ScratchBirdValidationBridge.diagnosticsFor(currentCommandText()),
            ScratchBirdValidationBridge.statementSummaryFor(currentCommandText()),
            ScratchBirdValidationBridge.formHintsFor(currentCommandText(), currentCommandText().length(), targetPath));
        setErrorMessage(null);
        refreshObjectEditorFields();
        refreshWorkflowFields();
        setMessage("ScratchBird preview validated locally; server authorization remains authoritative.");
    }

    private void refreshServerStatus() {
        ScratchBirdLiveProbe.ProbePlan activeAuthzPlan = activeAuthzProbePlan();
        if (!activeAuthzPlan.executable() && !probePlan.executable()) {
            setErrorMessage("No safe server-backed ScratchBird refresh probe is available for this form.");
            return;
        }
        final ScratchBirdLiveProbe.ProbeResult[] authzHolder = new ScratchBirdLiveProbe.ProbeResult[1];
        final ScratchBirdLiveProbe.ProbeResult[] liveHolder = new ScratchBirdLiveProbe.ProbeResult[1];
        try {
            UIUtils.runInProgressService(monitor -> {
                monitor.beginTask("ScratchBird management status refresh", 2);
                if (activeAuthzPlan.executable()) {
                    monitor.subTask("ScratchBird server authorization refresh");
                    authzHolder[0] = ScratchBirdLiveProbe.execute(monitor, targetObject, activeAuthzPlan);
                    monitor.worked(1);
                }
                if (probePlan.executable()) {
                    monitor.subTask("ScratchBird live state refresh");
                    liveHolder[0] = ScratchBirdLiveProbe.execute(monitor, targetObject, probePlan);
                    monitor.worked(1);
                }
                monitor.done();
            });
        } catch (InvocationTargetException e) {
            setErrorMessage("ScratchBird server status refresh failed: " + e.getTargetException().getMessage());
            return;
        } catch (InterruptedException e) {
            setMessage("ScratchBird server status refresh canceled.");
            return;
        }
        if (authzHolder[0] != null) {
            authzProbeResult = authzHolder[0];
            ScratchBirdProbeHistory.recordAuthorizationProbe(probeScopeKey, targetPath, form, authzProbeResult);
        }
        if (liveHolder[0] != null) {
            liveProbeResult = liveHolder[0];
            ScratchBirdProbeHistory.recordLiveProbe(probeScopeKey, targetPath, form, liveProbeResult);
        }
        selectedHistoryIndex = 0;
        setErrorMessage(null);
        refreshAuthzProbeFields();
        refreshLiveProbeFields();
        refreshWorkflowFields();
        refreshHistoryFields();
        setMessage(refreshStatusSummary());
    }

    private void applyAfterAdmission() {
        ScratchBirdRefusalModel readiness = applyReadiness();
        ScratchBirdAdminExecutor.ExecutionPlan activePlan = activeExecutionPlan();
        if (!readiness.isAdmitted()) {
            applyResult = ScratchBirdMutationApplyExecutor.refuse(activePlan, readiness, currentPreviewHash(), commandHash());
            ScratchBirdProbeHistory.recordApply(probeScopeKey, targetPath, form, applyResult);
            selectedHistoryIndex = 0;
            setErrorMessage("ScratchBird apply was not run. " + applyGateSummary());
            refreshWorkflowFields();
            refreshHistoryFields();
            return;
        }

        final ScratchBirdMutationApplyExecutor.ApplyResult[] applyHolder =
            new ScratchBirdMutationApplyExecutor.ApplyResult[1];
        final ScratchBirdLiveProbe.ProbeResult[] liveHolder = new ScratchBirdLiveProbe.ProbeResult[1];
        try {
            UIUtils.runInProgressService(monitor -> {
                monitor.beginTask("ScratchBird admitted management apply", probePlan.executable() ? 2 : 1);
                applyHolder[0] = ScratchBirdMutationApplyExecutor.apply(
                    monitor,
                    targetObject,
                    activePlan,
                    authzProbeResult,
                    currentPreviewHash(),
                    commandHash());
                monitor.worked(1);
                if (applyHolder[0].applied() && probePlan.executable()) {
                    monitor.subTask("ScratchBird post-apply refresh");
                    liveHolder[0] = ScratchBirdLiveProbe.execute(monitor, targetObject, probePlan);
                    monitor.worked(1);
                }
                monitor.done();
            });
        } catch (InvocationTargetException e) {
            setErrorMessage("ScratchBird admitted apply failed: " + e.getTargetException().getMessage());
            return;
        } catch (InterruptedException e) {
            setMessage("ScratchBird admitted apply canceled.");
            return;
        }

        applyResult = applyHolder[0];
        ScratchBirdProbeHistory.recordApply(probeScopeKey, targetPath, form, applyResult);
        if (liveHolder[0] != null) {
            liveProbeResult = liveHolder[0];
            ScratchBirdProbeHistory.recordLiveProbe(probeScopeKey, targetPath, form, liveProbeResult);
        }
        selectedHistoryIndex = 0;
        setErrorMessage(applyResult.applied() ? null : applyResult.status().message());
        refreshAuthzProbeFields();
        refreshLiveProbeFields();
        refreshWorkflowFields();
        refreshHistoryFields();
        setMessage(applyGateSummary());
    }

    private void refreshApplyButton() {
        if (applyButton == null || applyButton.isDisposed()) {
            return;
        }
        applyButton.setText(ScratchBirdManagementWorkflow.applyButtonLabel(applyButtonReady()));
        applyButton.setEnabled(applyButtonReady());
        setAccessibleName(applyButton, applyButton.getText());
    }

    private boolean applyButtonReady() {
        return ScratchBirdManagementWorkflow.applyButtonEnabled(activeExecutionPlan(), permission, authzProbeResult, applyResult,
            currentPreviewHash(), commandHash());
    }

    @NotNull
    private ScratchBirdRefusalModel applyReadiness() {
        if (permission.isDeterministicRefusal()) {
            return permission;
        }
        return ScratchBirdMutationApplyExecutor.applyReadiness(
            activeExecutionPlan(),
            authzProbeResult,
            currentPreviewHash(),
            commandHash());
    }

    @NotNull
    private String commandHash() {
        return ScratchBirdManagementActionEnvelope.commandHashFor(activeExecutionPlan());
    }

    private void refuseApply() {
        setErrorMessage("ScratchBird apply was not run. " + applyGateSummary());
        refreshWorkflowFields();
    }

    private void refreshWorkflowFields() {
        if (workflowStatusText != null && !workflowStatusText.isDisposed()) {
            workflowStatusText.setText(workflowStatusSummary());
        }
        if (workflowPreviewIdentityText != null && !workflowPreviewIdentityText.isDisposed()) {
            workflowPreviewIdentityText.setText(ScratchBirdManagementWorkflow.previewIdentity(activeExecutionPlan()));
        }
        if (workflowValidationText != null && !workflowValidationText.isDisposed()) {
            workflowValidationText.setText(localValidationSummary);
        }
        if (workflowApplyText != null && !workflowApplyText.isDisposed()) {
            workflowApplyText.setText(applyGateSummary());
        }
        if (workflowRefreshText != null && !workflowRefreshText.isDisposed()) {
            workflowRefreshText.setText(refreshStatusSummary());
        }
        if (workflowVerifyText != null && !workflowVerifyText.isDisposed()) {
            workflowVerifyText.setText(verifyStatusSummary());
        }
        if (workflowRefusalText != null && !workflowRefusalText.isDisposed()) {
            workflowRefusalText.setText(refusalSummary());
        }
        if (workflowRollbackText != null && !workflowRollbackText.isDisposed()) {
            workflowRollbackText.setText(rollbackSummary());
        }
        if (workflowLongOperationText != null && !workflowLongOperationText.isDisposed()) {
            workflowLongOperationText.setText(ScratchBirdManagementWorkflow.longOperationSummary());
        }
        if (workflowAuditText != null && !workflowAuditText.isDisposed()) {
            workflowAuditText.setText(auditSummary());
        }
        if (workflowFeatureBoundaryText != null && !workflowFeatureBoundaryText.isDisposed()) {
            workflowFeatureBoundaryText.setText(featureBoundarySummary());
        }
        refreshApplyButton();
    }

    private void refreshAuthzProbeFields() {
        if (authzStatusText != null && !authzStatusText.isDisposed()) {
            authzStatusText.setText(authzStatusSummary());
        }
        if (authzSummaryText != null && !authzSummaryText.isDisposed()) {
            authzSummaryText.setText(String.join("\n", activeAuthzProbePlan().summaryLines()));
        }
        if (authzCommandText != null && !authzCommandText.isDisposed()) {
            authzCommandText.setText(activeAuthzProbePlan().commandText());
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
        refreshManagementConsoleFields();
    }

    private void refreshManagementConsoleFields() {
        refreshManagementConsoleAgentFields();
        if (managementConsoleDetailText != null && !managementConsoleDetailText.isDisposed()) {
            managementConsoleDetailText.setText(managementConsoleSelectedDetail());
        }
        if (managementConsoleRuntimeText != null && !managementConsoleRuntimeText.isDisposed()) {
            managementConsoleRuntimeText.setText(managementConsoleRuntimeSummary());
        }
        if (managementConsolePolicyText != null && !managementConsolePolicyText.isDisposed()) {
            managementConsolePolicyText.setText(managementConsolePolicySummary());
        }
        if (managementConsoleActionHistoryText != null && !managementConsoleActionHistoryText.isDisposed()) {
            managementConsoleActionHistoryText.setText(managementConsoleActionHistorySummary());
        }
        if (managementConsoleEvidenceText != null && !managementConsoleEvidenceText.isDisposed()) {
            managementConsoleEvidenceText.setText(managementConsoleEvidenceSummary());
        }
        if (managementConsoleActionsText != null && !managementConsoleActionsText.isDisposed()) {
            managementConsoleActionsText.setText(managementConsoleActionsSummary());
        }
        if (managementConsoleLiveText != null && !managementConsoleLiveText.isDisposed()) {
            managementConsoleLiveText.setText(managementConsoleLiveSummary());
        }
        refreshManagementConsoleResultTables();
    }

    private void refreshManagementConsoleResultTables() {
        ScratchBirdManagementConsoleCatalog.ConsoleRow row = selectedManagementConsoleRow();
        String selectedKey = row == null ? "" : row.key();
        if ("management.agents".equals(consolePath())) {
            populateManagementResultTable(
                managementConsoleRuntimeResultTable,
                "Agent runtime rows",
                "sys.frontend.agents",
                "agent_type_id",
                selectedKey);
            populateManagementResultTable(
                managementConsoleMetricResultTable,
                "Agent metric dependencies",
                "sys.frontend.agent_metric_dependencies",
                "agent_name",
                selectedKey);
            populateManagementResultTable(
                managementConsolePolicyResultTable,
                "Agent policies",
                "sys.frontend.agent_policies",
                "agent_name",
                selectedKey);
            populateManagementResultTable(
                managementConsoleOverrideResultTable,
                "Agent overrides",
                "sys.frontend.agent_overrides",
                "target_name",
                selectedKey);
            populateManagementResultTable(
                managementConsoleActionResultTable,
                "Agent actions",
                "sys.frontend.agent_actions",
                "agent_name",
                selectedKey);
            populateManagementResultTable(
                managementConsoleActionAuditResultTable,
                "Agent audit",
                "sys.frontend.agent_audit",
                "",
                "");
            populateManagementResultTable(
                managementConsoleEvidenceResultTable,
                "Agent evidence",
                "sys.frontend.agent_evidence",
                "agent_name",
                selectedKey);
            populateManagementResultTable(
                managementConsoleEvidenceAuditResultTable,
                "Agent audit",
                "sys.frontend.agent_audit",
                "",
                "");
            refreshManagementConsoleAgentFields();
            refreshManagementConsolePolicySelector(selectedKey);
            refreshManagementConsolePolicyEditor();
            return;
        }

        String source = managementConsole != null && !managementConsole.monitoringSources().isEmpty()
            ? managementConsole.monitoringSources().get(0)
            : "";
        populateManagementResultTable(managementConsoleRuntimeResultTable, "Management rows", source, "", "");
        refreshManagementConsoleAgentFields();
        refreshManagementConsolePolicySelector(selectedKey);
        refreshManagementConsolePolicyEditor();
    }

    private void refreshManagementConsolePolicyEditor() {
        ScratchBirdManagementConsoleCatalog.ConsoleRow agent = selectedManagementConsoleRow();
        String agentName = agent == null ? "" : agent.label();
        ManagementResultRow policy = selectedManagementPolicyRow();
        String policyName = policy == null ? defaultPolicyName(agentName) : firstNonBlank(
            policy.value("policy_name"),
            defaultPolicyName(agentName));
        if (!"management.agents".equals(consolePath())) {
            policyName = policyNameForSelectedManagementItem(agentName);
        }
        setTextIfAvailable(managementConsolePolicyNameText, policyName);
        setTextIfAvailable(managementConsolePolicyFamilyText, policy == null ? defaultPolicyFamily(agentName) : firstNonBlank(policy.value("policy_family"), defaultPolicyFamily(agentName)));
        setTextIfAvailable(managementConsolePolicyVersionText, policy == null ? "draft" : policy.value("version_label"));
        setTextIfAvailable(managementConsolePolicyStateText, policy == null ? "draft" : policy.value("active_state"));
        setTextIfAvailable(managementConsolePolicyValidationText, policy == null ? "not validated" : policy.value("validation_state"));
        ManagementResultRow override = firstManagementResultRow("sys.frontend.agent_overrides", "target_name", agentName);
        setTextIfAvailable(managementConsolePolicyScheduleText, policy == null ?
            defaultPolicySchedule() :
            firstNonBlank(value(override, "state"), "active-policy driven"));
        setTextIfAvailable(managementConsolePolicyBodyText, policy == null ?
            defaultPolicyDocument(agentName, policyName) :
            policyRowsAsText(policy));
        if (managementConsolePolicyText != null && !managementConsolePolicyText.isDisposed()) {
            managementConsolePolicyText.setText(managementConsolePolicySummary());
        }
        if (managementConsolePolicySourceStatusText != null && !managementConsolePolicySourceStatusText.isDisposed()) {
            managementConsolePolicySourceStatusText.setText(policySourceStatus());
        }
        if (managementConsoleOverrideSourceStatusText != null && !managementConsoleOverrideSourceStatusText.isDisposed()) {
            managementConsoleOverrideSourceStatusText.setText(overrideSourceStatus());
        }
    }

    private void refreshManagementConsolePolicySelector(@NotNull String selectedKey) {
        if (managementConsolePolicySelector == null || managementConsolePolicySelector.isDisposed()) {
            return;
        }
        String selectedBefore = managementConsolePolicySelector.getText();
        String policySource = "management.agents".equals(consolePath()) ?
            "sys.frontend.agent_policies" :
            "";
        managementConsolePolicyRows = policySource.isBlank() ?
            List.of() :
            managementRows(policySource, "agent_name", selectedKey);

        managementConsolePolicySelector.removeAll();
        int selectedIndex = 0;
        if (managementConsolePolicyRows.isEmpty()) {
            managementConsolePolicySelector.add(policyNameForSelectedManagementItem(selectedKey) + " (draft)");
            managementConsolePolicySelector.select(0);
            return;
        }
        for (int i = 0; i < managementConsolePolicyRows.size(); i++) {
            ManagementResultRow row = managementConsolePolicyRows.get(i);
            String label = firstNonBlank(
                row.value("policy_name"),
                row.value("name"),
                policyNameForSelectedManagementItem(selectedKey));
            managementConsolePolicySelector.add(label);
            if (label.equals(selectedBefore)) {
                selectedIndex = i;
            }
        }
        managementConsolePolicySelector.select(Math.min(selectedIndex, managementConsolePolicySelector.getItemCount() - 1));
    }

    @Nullable
    private ManagementResultRow selectedManagementPolicyRow() {
        if (managementConsolePolicySelector == null || managementConsolePolicySelector.isDisposed()) {
            return selectedManagementResultRow(managementConsolePolicyResultTable);
        }
        int index = managementConsolePolicySelector.getSelectionIndex();
        return index >= 0 && index < managementConsolePolicyRows.size() ? managementConsolePolicyRows.get(index) : null;
    }

    @NotNull
    private List<ManagementResultRow> managementRows(
        @NotNull String source,
        @NotNull String filterColumn,
        @NotNull String filterValue
    ) {
        ScratchBirdLiveProbe.StatementResult result = findManagementResult(source);
        if (result == null || !result.resultSet()) {
            return List.of();
        }
        return filteredRows(result, filterColumn, filterValue).stream()
            .map(row -> new ManagementResultRow(result.columns(), row))
            .toList();
    }

    @Nullable
    private static ManagementResultRow selectedManagementResultRow(@Nullable Table table) {
        if (table == null || table.isDisposed()) {
            return null;
        }
        int index = table.getSelectionIndex();
        if (index < 0 || index >= table.getItemCount()) {
            return null;
        }
        Object data = table.getItem(index).getData();
        return data instanceof ManagementResultRow row ? row : null;
    }

    private void newAgentPolicyDraft() {
        ScratchBirdManagementConsoleCatalog.ConsoleRow agent = selectedManagementConsoleRow();
        String agentName = agent == null ? selectedPolicyTargetFallback() : agent.label();
        String policyName = policyNameForSelectedManagementItem(agentName);
        setTextIfAvailable(managementConsolePolicyNameText, policyName);
        setTextIfAvailable(managementConsolePolicyFamilyText, defaultPolicyFamily(agentName));
        setTextIfAvailable(managementConsolePolicyVersionText, "draft");
        setTextIfAvailable(managementConsolePolicyStateText, "draft");
        setTextIfAvailable(managementConsolePolicyValidationText, "not validated");
        setTextIfAvailable(managementConsolePolicyScheduleText, defaultPolicySchedule());
        setTextIfAvailable(managementConsolePolicyBodyText, defaultPolicyDocument(agentName, policyName));
        managementConsoleMutationCommand = "";
        authzProbeResult = null;
        applyResult = null;
        refreshWorkflowFields();
        setMessage("New policy draft started for " + agentName + ". Use Validate Policy before Set Active.");
    }

    private void prepareAgentPolicyCommand(@NotNull String operation) {
        ScratchBirdManagementConsoleCatalog.ConsoleRow agent = selectedManagementConsoleRow();
        if (agent == null) {
            setErrorMessage("Select an agent before preparing a policy command.");
            return;
        }
        String policyName = textValue(managementConsolePolicyNameText);
        if (policyName.isBlank()) {
            setErrorMessage("Enter or select a policy name before preparing a policy command.");
            return;
        }
        managementConsoleMutationCommand = policyMutationCommand(agent, operation, policyName);
        authzProbeResult = null;
        applyResult = null;
        setErrorMessage(null);
        refreshWorkflowFields();
        refreshApplyButton();
        setMessage(operationTitle(operation) + " prepared for " + agent.label() + "/" + policyName +
            ". Run Authz Probe, then Apply when admitted.");
    }

    @NotNull
    private String policyMutationCommand(
        @NotNull ScratchBirdManagementConsoleCatalog.ConsoleRow target,
        @NotNull String operation,
        @NotNull String policyName
    ) {
        if ("management.agents".equals(consolePath())) {
            return "ALTER AGENT " + quoteIdentifier(target.label()) + " " +
                operation + " POLICY " + quoteIdentifier(policyName);
        }
        String surface = consolePath().isBlank() ? "management" : consolePath();
        return "ALTER MANAGEMENT " + quoteIdentifier(surface) + " " +
            operation + " POLICY " + quoteIdentifier(policyName) +
            " FOR " + quoteIdentifier(target.label());
    }

    @NotNull
    private String selectedPolicyTargetFallback() {
        ScratchBirdManagementConsoleCatalog.ConsoleRow row = selectedManagementConsoleRow();
        return row == null ? "management" : row.label();
    }

    private void refreshManagementConsoleAgentFields() {
        ScratchBirdManagementConsoleCatalog.ConsoleRow agent = selectedManagementConsoleRow();
        String agentName = agent == null ? "" : agent.label();
        if (!"management.agents".equals(consolePath())) {
            setTextIfAvailable(managementConsoleAgentNameText, agentName);
            setTextIfAvailable(managementConsoleAgentLayerText, agent == null ? "" : agent.kind());
            setTextIfAvailable(managementConsoleAgentScopeText, agent == null ? "" : agent.source());
            setTextIfAvailable(managementConsoleAgentStateText, agent == null ? "" : agent.state());
            setTextIfAvailable(managementConsoleAgentHealthText, managementSurface == null ? "server-backed" : managementSurface.displayType());
            setTextIfAvailable(managementConsoleAgentEnabledText, agent != null && agent.enabled() ? "true" : "false");
            setTextIfAvailable(managementConsoleAgentActivePolicyText, policyNameForSelectedManagementItem(agentName));
            setTextIfAvailable(managementConsoleAgentLastActivityText, liveProbeResult == null ? "not refreshed" : liveProbeResult.status().kind().name());
            return;
        }
        ManagementResultRow runtime = firstManagementResultRow("sys.frontend.agents", "agent_type_id", agentName);
        ManagementResultRow action = firstManagementResultRow("sys.frontend.agent_actions", "agent_name", agentName);
        ManagementResultRow evidence = firstManagementResultRow("sys.frontend.agent_evidence", "agent_name", agentName);
        ManagementResultRow audit = firstManagementResultRow("sys.frontend.agent_audit", "", "");

        setTextIfAvailable(managementConsoleAgentNameText, firstNonBlank(value(runtime, "agent_name"), agentName));
        setTextIfAvailable(managementConsoleAgentLayerText, agent == null ? "" : agent.kind());
        setTextIfAvailable(managementConsoleAgentScopeText, firstNonBlank(value(runtime, "scope_kind"), agent == null ? "" : agent.source()));
        setTextIfAvailable(managementConsoleAgentStateText, firstNonBlank(value(runtime, "state"), agent == null ? "" : agent.state()));
        setTextIfAvailable(managementConsoleAgentHealthText, firstNonBlank(value(runtime, "health_state"), "not published"));
        setTextIfAvailable(managementConsoleAgentEnabledText, firstNonBlank(value(runtime, "enabled"), agent != null && agent.enabled() ? "true" : "false"));
        setTextIfAvailable(managementConsoleAgentActivePolicyText, firstNonBlank(value(runtime, "policy_name"), "no active policy row visible"));
        setTextIfAvailable(managementConsoleAgentLastActivityText, firstNonBlank(
            value(action, "action_id"),
            value(evidence, "created_at"),
            value(audit, "created_at"),
            "no recent activity row visible"));
    }

    @Nullable
    private ManagementResultRow firstManagementResultRow(
        @NotNull String source,
        @NotNull String filterColumn,
        @NotNull String filterValue
    ) {
        ScratchBirdLiveProbe.StatementResult result = findManagementResult(source);
        if (result == null || !result.resultSet()) {
            return null;
        }
        List<List<String>> rows = filteredRows(result, filterColumn, filterValue);
        if (rows.isEmpty()) {
            return null;
        }
        return new ManagementResultRow(result.columns(), rows.get(0));
    }

    @NotNull
    private static String value(@Nullable ManagementResultRow row, @NotNull String columnName) {
        return row == null ? "" : row.value(columnName);
    }

    private static void setTextIfAvailable(@Nullable Text text, @NotNull String value) {
        if (text != null && !text.isDisposed()) {
            text.setText(value);
        }
    }

    @NotNull
    private static String textValue(@Nullable Text text) {
        return text == null || text.isDisposed() ? "" : text.getText().trim();
    }

    @NotNull
    private static String firstNonBlank(@NotNull String... values) {
        for (String value : values) {
            if (value != null && !value.isBlank() && !"NULL".equalsIgnoreCase(value)) {
                return value;
            }
        }
        return "";
    }

    @NotNull
    private static String defaultPolicyName(@NotNull String agentName) {
        return (agentName.isBlank() ? "agent" : agentName) + "_policy";
    }

    @NotNull
    private String policyNameForSelectedManagementItem(@NotNull String itemName) {
        String base = itemName.isBlank() ? consolePath().replace('.', '_') : itemName;
        if (base.isBlank()) {
            base = "management";
        }
        return normalizedIdentifier(base) + "_policy";
    }

    @NotNull
    private String defaultPolicyFamily(@NotNull String itemName) {
        String path = consolePath().isBlank() ? "management" : consolePath();
        String base = itemName.isBlank() ? path : path + "_" + itemName;
        return normalizedIdentifier(base) + "_policy";
    }

    @NotNull
    private String defaultPolicySchedule() {
        return managementSurface == null ? "manual until saved/attached" : managementSurface.refreshPolicy();
    }

    @NotNull
    private String defaultPolicyDocument(
        @NotNull String itemName,
        @NotNull String policyName
    ) {
        String surfacePath = consolePath().isBlank() ? "management" : consolePath();
        return "surface=" + surfacePath +
            "\nitem=" + itemName +
            "\npolicy=" + policyName +
            "\nstate=draft" +
            "\nrequired_permission=" + (managementSurface == null ? "unknown" : managementSurface.requiredPermission()) +
            "\nrefresh_policy=" + defaultPolicySchedule();
    }

    @NotNull
    private String policySourceStatus() {
        ScratchBirdManagementConsoleCatalog.ConsoleRow row = selectedManagementConsoleRow();
        String selected = row == null ? "no selected item" : row.label();
        if ("management.agents".equals(consolePath())) {
            if (liveProbeResult == null) {
                return "Visible policy documents for " + selected + " have not been refreshed yet.";
            }
            if (managementConsolePolicyRows.isEmpty()) {
                return "No visible server policy document is published for " + selected + ". The editor is showing a draft document.";
            }
            return managementConsolePolicyRows.size() + " visible policy document(s) loaded for " + selected + ".";
        }
        return "This management branch uses the same policy editor contract. The selected item's effective policy/configuration is edited as a document and must be admitted by the server before apply.";
    }

    @NotNull
    private String overrideSourceStatus() {
        ScratchBirdManagementConsoleCatalog.ConsoleRow row = selectedManagementConsoleRow();
        String selected = row == null ? "no selected item" : row.label();
        if (!"management.agents".equals(consolePath())) {
            return "Schedule and override state for " + selected + " is represented in the policy document until a branch-specific override source is published.";
        }
        ManagementResultRow override = firstManagementResultRow("sys.frontend.agent_overrides", "target_name", selected);
        return override == null ?
            "No visible override row is published for " + selected + "." :
            policyRowsAsText(override);
    }

    @NotNull
    private static String normalizedIdentifier(@NotNull String value) {
        String normalized = value.toLowerCase(Locale.ENGLISH).replaceAll("[^a-z0-9_]+", "_");
        normalized = normalized.replaceAll("_+", "_").replaceAll("^_|_$", "");
        return normalized.isBlank() ? "management" : normalized;
    }

    @NotNull
    private static String policyRowsAsText(@NotNull ManagementResultRow policy) {
        List<String> lines = new ArrayList<>();
        for (int i = 0; i < policy.columns().size(); i++) {
            String value = i < policy.values().size() ? policy.values().get(i) : "";
            lines.add(policy.columns().get(i) + "=" + value);
        }
        return String.join("\n", lines);
    }

    @NotNull
    private static String operationTitle(@NotNull String operation) {
        return switch (operation) {
            case "ATTACH" -> "Set active policy";
            case "VALIDATE" -> "Validate policy";
            case "SIMULATE" -> "Simulate policy";
            case "APPLY" -> "Apply policy";
            default -> operation.substring(0, 1).toUpperCase(Locale.ENGLISH) +
                operation.substring(1).toLowerCase(Locale.ENGLISH);
        };
    }

    @NotNull
    private static String quoteIdentifier(@NotNull String value) {
        if (value.matches("[A-Za-z_][A-Za-z0-9_]*")) {
            return value;
        }
        return "\"" + value.replace("\"", "\"\"") + "\"";
    }

    private void refreshObjectEditorFields() {
        if (objectEditorCreateText != null && !objectEditorCreateText.isDisposed()) {
            objectEditorCreateText.setText(lifecyclePreview(ScratchBirdFormMode.CREATE));
        }
        if (objectEditorAlterText != null && !objectEditorAlterText.isDisposed()) {
            objectEditorAlterText.setText(lifecyclePreview(ScratchBirdFormMode.ALTER));
        }
        if (objectEditorDropText != null && !objectEditorDropText.isDisposed()) {
            objectEditorDropText.setText(lifecyclePreview(ScratchBirdFormMode.DELETE));
        }
        if (objectEditorValidationText != null && !objectEditorValidationText.isDisposed() && objectEditor != null) {
            objectEditorValidationText.setText(objectEditorValidationSummary(objectEditor));
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
    private String workflowStatusSummary() {
        return ScratchBirdManagementWorkflow.workflowStatus(
            activeExecutionPlan(),
            permission,
            authzProbeResult,
            liveProbeResult,
            taskProbeResult);
    }

    @NotNull
    private String applyGateSummary() {
        return ScratchBirdManagementWorkflow.applyGateSummary(
            activeExecutionPlan(),
            permission,
            authzProbeResult,
            applyResult,
            currentPreviewHash(),
            commandHash());
    }

    @NotNull
    private String refreshStatusSummary() {
        return ScratchBirdManagementWorkflow.refreshStatusSummary(
            activeAuthzProbePlan(),
            probePlan,
            authzProbeResult,
            liveProbeResult);
    }

    @NotNull
    private String verifyStatusSummary() {
        return ScratchBirdManagementWorkflow.verifyStatusSummary(activeExecutionPlan(), authzProbeResult, liveProbeResult, applyResult);
    }

    @NotNull
    private String refusalSummary() {
        return ScratchBirdManagementWorkflow.refusalSummary(form, permission, report);
    }

    @NotNull
    private String rollbackSummary() {
        return ScratchBirdManagementWorkflow.rollbackSummary(destructivePlan);
    }

    @NotNull
    private String auditSummary() {
        return ScratchBirdManagementWorkflow.auditSummary(probeScopeKey, targetPath);
    }

    @NotNull
    private String featureBoundarySummary() {
        return ScratchBirdManagementWorkflow.featureBoundarySummary(form, permission);
    }

    @NotNull
    private String authzStatusSummary() {
        ScratchBirdLiveProbe.ProbePlan activeAuthzPlan = activeAuthzProbePlan();
        if (authzProbeResult == null) {
            return activeAuthzPlan.executable() ?
                "Not yet executed. Use Run Authz Probe to verify server-backed capability inventory and branch authorization." :
                "No safe server-backed authz probe is available for this form.";
        }
        if (authzProbeResult.status().isAdmitted()) {
            return activeAuthzPlan.surrogate() ?
                "ADMITTED: Server-backed read-only authz probe completed; mutation apply still requires permission-probe admission." :
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

    @Nullable
    private ScratchBirdManagementConsoleCatalog.ConsoleRow selectedManagementConsoleRow() {
        if (managementConsoleTable == null || managementConsoleTable.isDisposed()) {
            return null;
        }
        int index = managementConsoleTable.getSelectionIndex();
        if (index < 0 || index >= managementConsoleTable.getItemCount()) {
            return null;
        }
        Object data = managementConsoleTable.getItem(index).getData();
        return data instanceof ScratchBirdManagementConsoleCatalog.ConsoleRow row ? row : null;
    }

    @NotNull
    private String managementConsoleSelectedDetail() {
        ScratchBirdManagementConsoleCatalog.ConsoleRow row = selectedManagementConsoleRow();
        if (row == null) {
            return "Select an agent to inspect its runtime state, policy posture, actions, and evidence.";
        }
        return row.detail() + "\nStatus: " + (row.enabled() ? "enabled/selectable" : "disabled/selectable for inspection");
    }

    @NotNull
    private String managementConsoleRuntimeSummary() {
        ScratchBirdManagementConsoleCatalog.ConsoleRow row = selectedManagementConsoleRow();
        if (row == null) {
            return "Select a management row to inspect runtime status.";
        }
        if (!"management.agents".equals(consolePath())) {
            return "Runtime data is shown in the table below when the server publishes rows for this management surface.";
        }
        return "Default state: " + row.state() +
            "\nScope: " + row.source() +
            "\nThe runtime table shows current state, health, enabled flag, and active policy for the selected agent." +
            "\nRefresh state: " + (liveProbeResult == null ? "not refreshed" : liveProbeResult.status().kind() + ": " + liveProbeResult.status().message());
    }

    @NotNull
    private String managementConsolePolicySummary() {
        ScratchBirdManagementConsoleCatalog.ConsoleRow row = selectedManagementConsoleRow();
        if (row == null) {
            return "Select a management row to inspect policy and schedule posture.";
        }
        if (!"management.agents".equals(consolePath())) {
            return "Edit the selected management item's policy/configuration as a document. Apply remains disabled until the server admits the exact generated command.";
        }
        return "Select a visible policy document or edit the draft policy for the selected agent." +
            "\nSchedule posture is shown from active policy and visible overrides when present." +
            "\nUse Validate, Simulate, Set Active, or Apply to prepare an admitted server action for the selected policy.";
    }

    @NotNull
    private String managementConsoleActionHistorySummary() {
        ScratchBirdManagementConsoleCatalog.ConsoleRow row = selectedManagementConsoleRow();
        if (row == null) {
            return "Select a management row to inspect action history.";
        }
        if (!"management.agents".equals(consolePath())) {
            return "Action rows are shown below when published by the server. Actions are preview-only until server admission.";
        }
        return "Pending and recent actions are shown below for the selected agent." +
            "\nActions remain guarded until the server admits the exact command for the connected session.";
    }

    @NotNull
    private String managementConsoleEvidenceSummary() {
        ScratchBirdManagementConsoleCatalog.ConsoleRow row = selectedManagementConsoleRow();
        if (row == null) {
            return "Select a management row to inspect evidence and audit visibility.";
        }
        if (!"management.agents".equals(consolePath())) {
            return "Evidence rows are shown below when published by the server. Hidden evidence remains refused by the server.";
        }
        return "Evidence and audit rows are shown below for the selected agent." +
            "\nSensitive fields remain redacted unless the connected session is authorized to view them.";
    }

    @NotNull
    private String managementConsoleActionsSummary() {
        if (managementConsole == null) {
            return "-";
        }
        ScratchBirdManagementConsoleCatalog.ConsoleRow row = selectedManagementConsoleRow();
        String selected = row == null ? "No selected row." : "Selected: " + row.label() + " (" + row.state() + ")";
        return selected + "\n" + String.join("\n", managementConsole.mutationActions()) +
            "\nAll mutating operations require ScratchBird server admission for the exact command, session, role, policy epoch, and target UUID.";
    }

    @NotNull
    private String managementConsoleLiveSummary() {
        ScratchBirdManagementConsoleCatalog.ConsoleRow row = selectedManagementConsoleRow();
        String selected = row == null ? "No selected row." : "Selected item: " + row.label();
        if (liveProbeResult == null) {
            return selected + "\nNo live management data captured yet. Use Run Live Probe or Refresh Server Status.";
        }
        return selected +
            "\nStatus: " + liveProbeResult.status().kind() + ": " + liveProbeResult.status().redactedMessage() +
            "\nResult sets loaded: " + liveProbeResult.statementResults().size() +
            "\nRows are displayed in Runtime, Actions, and Evidence tables; policies are opened in the policy document editor.";
    }

    @NotNull
    private String consolePath() {
        return managementConsole == null ? "" : managementConsole.path();
    }

    @NotNull
    private static String agentSourceQuery(@NotNull String source, @NotNull String agentTypeId) {
        if ("sys.frontend.agent_audit".equals(source)) {
            return "SELECT * FROM sys.frontend.agent_audit";
        }
        String column = switch (source) {
            case "sys.frontend.agents" -> "agent_type_id";
            case "sys.frontend.agent_overrides" -> "target_name";
            default -> "agent_name";
        };
        return "SELECT * FROM " + source + " WHERE " + column + " = '" + agentTypeId.replace("'", "''") + "'";
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
    private String currentCommandText() {
        if (!managementConsoleMutationCommand.isBlank()) {
            return managementConsoleMutationCommand;
        }
        if (objectEditorDraftText != null && !objectEditorDraftText.isDisposed()) {
            String draft = objectEditorDraftText.getText();
            if (draft != null && !draft.isBlank()) {
                return draft;
            }
        }
        return plan.commandText();
    }

    @NotNull
    private ScratchBirdAdminExecutor.ExecutionPlan activeExecutionPlan() {
        return new ScratchBirdAdminExecutor.ExecutionPlan(
            plan.form(),
            plan.mode(),
            plan.targetPath(),
            currentCommandText(),
            plan.executable(),
            plan.destructive(),
            plan.authority());
    }

    @NotNull
    private ScratchBirdLiveProbe.ProbePlan activeAuthzProbePlan() {
        return ScratchBirdPermissionProbe.planServerAuthorization(
            form,
            mode,
            targetPath,
            activeExecutionPlan(),
            taskDefinitions,
            destructivePlan);
    }

    @NotNull
    private String currentPreviewHash() {
        return ScratchBirdManagementActionEnvelope.previewHashFor(form, mode, targetPath, activeExecutionPlan());
    }

    @NotNull
    private String lifecyclePreview(@NotNull ScratchBirdFormMode requestedMode) {
        if (!form.supportsMode(requestedMode)) {
            return "Unsupported by " + form.id();
        }
        return ScratchBirdAdminExecutor.plan(form, requestedMode, targetPath).commandText();
    }

    @NotNull
    private String objectEditorValidationSummary(@NotNull ScratchBirdSqlObjectEditorCatalog.EditorDefinition editor) {
        return String.join("\n", editor.validationRules()) +
            "\nActive preview hash: " + currentPreviewHash() +
            "\nActive command hash: " + commandHash() +
            "\nActive authz probe: " + activeAuthzProbePlan().label();
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
