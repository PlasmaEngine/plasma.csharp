#pragma once

#include <EditorPluginCSharp/EditorPluginCSharpDLL.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.moc.h>

class plCSharpProjectAssetDocument;
class QTextEdit;
struct plDocumentObjectPropertyEvent;

class PL_EDITORPLUGINCSHARP_DLL plQtCSharpProjectAssetDocumentWindow : public plQtDocumentWindow
{
  Q_OBJECT

public:
  explicit plQtCSharpProjectAssetDocumentWindow(plCSharpProjectAssetDocument* pDocument);
  ~plQtCSharpProjectAssetDocumentWindow();

private:
  void DocumentObjectEventHandler(const plDocumentObjectPropertyEvent& event);
  void UpdateDiagnostics();

  plCSharpProjectAssetDocument* m_pAssetDocument = nullptr;
  QTextEdit* m_pDiagnostics = nullptr;
};
