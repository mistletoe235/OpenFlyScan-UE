#include "GaussianSplatComponentDetails.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Editor.h"
#include "GaussianSplatComponent.h"
#include "LevelEditorViewport.h"
#include "Widgets/Input/SButton.h"

#define LOCTEXT_NAMESPACE "FGaussianSplatComponentDetails"

TSharedRef<IDetailCustomization> FGaussianSplatComponentDetails::MakeInstance()
{
	return MakeShared<FGaussianSplatComponentDetails>();
}

void FGaussianSplatComponentDetails::CustomizeDetails(IDetailLayoutBuilder& DetailLayout)
{
	TArray<TWeakObjectPtr<UObject>> CustomizedObjects;
	DetailLayout.GetObjectsBeingCustomized(CustomizedObjects);

	TWeakObjectPtr<UGaussianSplatComponent> Component;
	for (const TWeakObjectPtr<UObject>& Object : CustomizedObjects)
	{
		if (UGaussianSplatComponent* Candidate = Cast<UGaussianSplatComponent>(Object.Get()))
		{
			Component = Candidate;
			break;
		}
	}

	IDetailCategoryBuilder& Category =
		DetailLayout.EditCategory(TEXT("Gaussian Splatting|Tree LOD"));
	Category.AddCustomRow(LOCTEXT("FocusSearch", "Focus NanoGS"))
	.WholeRowContent()
	[
		SNew(SButton)
		.Text(LOCTEXT("FocusSelected", "Focus Viewport on this NanoGS"))
		.ToolTipText(LOCTEXT("FocusSelectedTip", "Frame this NanoGS component in the active level viewport."))
		.IsEnabled_Lambda([Component] { return Component.IsValid(); })
		.OnClicked_Lambda([Component]
		{
			if (!GEditor || !Component.IsValid() || !GEditor->GetActiveViewport())
				return FReply::Handled();

			for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
			{
				if (Client && Client->Viewport == GEditor->GetActiveViewport())
				{
					Client->FocusViewportOnBox(Component->Bounds.GetBox(), false);
					break;
				}
			}
			return FReply::Handled();
		})
	];
}

#undef LOCTEXT_NAMESPACE
