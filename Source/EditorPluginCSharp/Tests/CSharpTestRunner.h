#pragma once

/// Registers the C# runner of the project checks' ScriptTests kind.
///
/// Every C# script project of the open project (a .csproj that uses Plasma.ScriptCore) is built into a scratch folder
/// and its [PlasmaTest] methods run in the packaged Plasma.ScriptInspector tool. Tests run without the engine, so they
/// test script logic. Build errors and failed asserts become problems at their file and line.
void plRegisterCSharpTestRunner();
void plUnregisterCSharpTestRunner();
