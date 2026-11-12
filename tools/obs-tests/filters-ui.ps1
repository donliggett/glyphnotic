# Adds and removes Glyphnotic filters through OBS's own Filters window, as a
# user would, with Windows UI Automation and real mouse clicks (Phase 5's
# leak test; run_obs_tests.py's leak-ui test starts OBS and runs this).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File filters-ui.ps1 -Source "P5 leak" -Cycles 100
#
# OBS must be running with the source in the current scene. For each cycle
# it clicks + in the source's Filters window, picks the next Glyphnotic
# filter (Ink, Mosaic, Poster, Console in turn), accepts the default name,
# waits, then selects the filter and clicks - and confirms. It moves the
# mouse: leave the mouse and keyboard alone while it runs. Writes one line
# per cycle and "filters-ui: done <n>" at the end; exits 1 on a failure.
# It's a test tool and is never shipped.
param(
    [string] $Source = 'P5 leak',
    [int] $Cycles = 100,
    [int] $ShowMs = 300
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Mouse {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
    public static void Click(int x, int y) {
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(30);
        mouse_event(0x2, 0, 0, 0, UIntPtr.Zero);
        mouse_event(0x4, 0, 0, 0, UIntPtr.Zero);
    }
    public static void Key(byte vk) {
        keybd_event(vk, 0, 0, UIntPtr.Zero);
        keybd_event(vk, 0, 2, UIntPtr.Zero);
    }
}
'@

$AE = [System.Windows.Automation.AutomationElement]
$Scope = [System.Windows.Automation.TreeScope]
$Kinds = @('Glyphnotic Ink', 'Glyphnotic Mosaic', 'Glyphnotic Poster', 'Glyphnotic Console')

function Cond($prop, $value) { New-Object System.Windows.Automation.PropertyCondition($prop, $value) }

function WaitFor([scriptblock] $find, [string] $what, [int] $ms = 5000) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ($t.ElapsedMilliseconds -lt $ms) {
        # Qt rebuilds its accessibility nodes as the window changes, so a lookup can hit a stale node: retry.
        try { $e = & $find } catch { $e = $null }
        if ($e) { return $e }
        Start-Sleep -Milliseconds 50
    }
    throw "timed out waiting for $what"
}

function ClickOn($e) {
    $r = $e.Current.BoundingRectangle
    [Mouse]::Click([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2))
}

$obs = Get-Process obs64
$pidCond = Cond $AE::ProcessIdProperty $obs.Id
$main = $AE::FromHandle($obs.MainWindowHandle)
[Mouse]::SetForegroundWindow($obs.MainWindowHandle) | Out-Null

# The source's Filters window.
$item = WaitFor { $main.FindFirst($Scope::Descendants, (New-Object System.Windows.Automation.AndCondition(
    (Cond $AE::NameProperty $Source), (Cond $AE::ControlTypeProperty ([System.Windows.Automation.ControlType]::ListItem))))) } "source '$Source'"
$item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
$filtersButton = $main.FindFirst($Scope::Descendants,
    (Cond $AE::AutomationIdProperty 'OBSApp.OBSBasic.centralwidget.contextContainer.contextSubContainer.sourceFiltersButton'))
ClickOn $filtersButton
# Looked up again on every use (see WaitFor).
function ById([string] $id) {
    WaitFor { $main.FindFirst($Scope::Descendants, (Cond $AE::AutomationIdProperty $id)) } $id
}
$addId = 'OBSApp.OBSBasicFilters.effectWidget.widget_2.addEffectFilter'
$removeId = 'OBSApp.OBSBasicFilters.effectWidget.widget_2.removeEffectFilter'
$listId = 'OBSApp.OBSBasicFilters.effectWidget.effectFilters'
ById $addId | Out-Null

function FindTop([string] $name, $type) {
    # Menus and dialogs: top-level windows of OBS's process, or children of the main window.
    foreach ($w in $AE::RootElement.FindAll($Scope::Children, $pidCond)) {
        $e = $w.FindFirst($Scope::Subtree, (New-Object System.Windows.Automation.AndCondition(
            (Cond $AE::NameProperty $name), (Cond $AE::ControlTypeProperty $type))))
        if ($e) { return $e }
    }
    return $null
}

function Rows { (ById $listId).FindAll($Scope::Children, [System.Windows.Automation.Condition]::TrueCondition) }
function FilterCount { (Rows).Count }

function Attempt([scriptblock] $click, [scriptblock] $find, [string] $what) {
    # A click can land while Qt is still laying the window out: click again, up to 3 times.
    for ($try = 1; $try -le 3; $try++) {
        & $click
        try { return (WaitFor $find $what 3000) } catch { if ($try -eq 3) { throw } }
    }
}

$base = FilterCount
for ($n = 1; $n -le $Cycles; $n++) {
    $kind = $Kinds[($n - 1) % $Kinds.Count]

    # + -> the filter's menu item -> OK in the name dialog (Enter accepts the default name).
    $menuItem = Attempt { ClickOn (ById $addId) } { FindTop $kind ([System.Windows.Automation.ControlType]::MenuItem) } "menu item $kind"
    Attempt { ClickOn $menuItem } { FindTop 'OK' ([System.Windows.Automation.ControlType]::Button) } 'the name dialog' | Out-Null
    [Mouse]::Key(0x0D)
    WaitFor { if ((FilterCount) -eq $base + 1) { $true } } "filter $n in the list" 10000 | Out-Null
    Start-Sleep -Milliseconds $ShowMs

    # Select it, - , Yes.
    $yes = Attempt {
        $rows = WaitFor { Rows } 'the filter list'
        ClickOn $rows[$rows.Count - 1]
        Start-Sleep -Milliseconds 150
        ClickOn (ById $removeId)
    } { FindTop 'Yes' ([System.Windows.Automation.ControlType]::Button) } 'the remove confirmation'
    # Once only: a second click where the dialog was would land in the Filters window.
    ClickOn $yes
    WaitFor { if ((FilterCount) -eq $base) { $true } } "filter $n removed" 10000 | Out-Null
    Write-Output ("filters-ui: {0} {1} added and removed" -f $n, $kind)
}
Write-Output "filters-ui: done $Cycles"
