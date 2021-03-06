// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	SkeletalMeshMerge.cpp: Unreal skeletal mesh merging implementation.
=============================================================================*/

#include "CustomSkeletalMeshMerge.h"
#include "GPUSkinPublicDefs.h"
#include "RawIndexBuffer.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"

#include "ImageUtils.h"
#include "FileHelper.h"
#include "Engine.h"

/*-----------------------------------------------------------------------------
	FCustomSkeletalMeshMerge
-----------------------------------------------------------------------------*/
#pragma optimize("", off)

static TAutoConsoleVariable<int32> CVarSaveIntermediateTextures(
	TEXT("SkeletalMeshMerge.SaveIntermediateTextures"),
	0,
	TEXT("Determines whether or not to save out intermediate BMP images for each flattened material property.\n")
	TEXT("0: Turned Off\n")
	TEXT("1: Turned On"),
	ECVF_Default);

/**
* Constructor
* @param InMergeMesh - destination mesh to merge to
* @param InSrcMeshList - array of source meshes to merge
* @param InForceSectionMapping - optional array to map sections from the source meshes to merged section entries
*/
FCustomSkeletalMeshMerge::FCustomSkeletalMeshMerge(USkeletalMesh* InMergeMesh,
	UMaterialInterface* InBaseMaterial,
	const TArray<FSkelMeshMergePart>& InSrcMeshList,
	const TArray<FSkelMeshMergeSectionMapping>& InForceSectionMapping,
	int32 InStripTopLODs,
	EMeshBufferAccess InMeshBufferAccess)
	: MergeMesh(InMergeMesh)
	, BaseMaterial(InBaseMaterial)
	, StripTopLODs(InStripTopLODs)
	, MeshBufferAccess(InMeshBufferAccess)
	, ForceSectionMapping(InForceSectionMapping)
{
	check(MergeMesh);

	for (int i = 0; i < InSrcMeshList.Num(); i++)
	{
		SrcMeshList.Add(InSrcMeshList[i].SkeletalMesh);
		SrcMeshAttachedBoneNameList.Add(InSrcMeshList[i].AttachedBoneName);
		VerticesTransformList.Add(InSrcMeshList[i].VerticesTransform);
	}
}

/** Helper macro to call GenerateLODModel which requires compile time vertex type. */
#define GENERATE_LOD_MODEL( VertexType, NumUVs, bHasExtraBoneInfluences ) \
{\
	switch( NumUVs )\
	{\
	case 1:\
		GenerateLODModel< VertexType<1>, TSkinWeightInfo<bHasExtraBoneInfluences> >( LODIdx + StripTopLODs );\
		break;\
	case 2:\
		GenerateLODModel< VertexType<2>, TSkinWeightInfo<bHasExtraBoneInfluences> >( LODIdx + StripTopLODs );\
		break;\
	case 3:\
		GenerateLODModel< VertexType<3>, TSkinWeightInfo<bHasExtraBoneInfluences> >( LODIdx + StripTopLODs );\
		break;\
	case 4:\
		GenerateLODModel< VertexType<4>, TSkinWeightInfo<bHasExtraBoneInfluences> >( LODIdx + StripTopLODs );\
		break;\
	default:\
		checkf(false, TEXT("Invalid number of UV sets.  Must be between 0 and 4") );\
		break;\
	}\
}\

/**
* Merge/Composite the list of source meshes onto the merge one
* The MergeMesh is reinitialized
* @return true if succeeded
*/
bool FCustomSkeletalMeshMerge::DoMerge(TArray<FRefPoseOverride>* RefPoseOverrides /* = nullptr */)
{
	MergeMaterial();
	MergeSkeleton(RefPoseOverrides);

	return FinalizeMesh();
}

namespace
{
	void GeneratedBinnedTextureSquares(const FVector2D DestinationSize, TArray<FVector2D>& InTexureSize, TArray<FBox2D>& OutGeneratedBoxes)
	{
		typedef FBox2D FTextureArea;
		struct FWeightedTexture
		{
			FTextureArea Area;
			int32 TextureIndex;
			float Weight;
		};

		TArray<FWeightedTexture> WeightedTextures;
		const float TotalArea = DestinationSize.X * DestinationSize.Y;
		// Generate textures with their size calculated according to their weight
		for (int32 TextureIndex = 0; TextureIndex < InTexureSize.Num(); ++TextureIndex)
		{
			FWeightedTexture Texture;
			const FVector2D& TextureSize = InTexureSize[TextureIndex];
			Texture.Area = FTextureArea(FVector2D(0.0f, 0.0f), TextureSize);
			Texture.TextureIndex = TextureIndex;
			Texture.Weight = TextureSize.X / DestinationSize.X;
			WeightedTextures.Add(Texture);
		}

		// Sort textures by their weight (high to low) which influences the insert order
		WeightedTextures.Sort([](const FWeightedTexture& One, const FWeightedTexture& Two) { return One.Weight > Two.Weight; });

		TArray<FWeightedTexture> InsertedTextures;
		typedef FBox2D FUnusedArea;
		TArray<FUnusedArea> UnusedAreas;

		bool bSuccess = true;
		do
		{
			// Reset state
			bSuccess = true;
			UnusedAreas.Empty();
			InsertedTextures.Empty();
			FUnusedArea StartArea(FVector2D(0, 0), DestinationSize);
			UnusedAreas.Add(StartArea);

			for (const FWeightedTexture& Texture : WeightedTextures)
			{
				int32 BestAreaIndex = -1;
				float RemainingArea = FLT_MAX;
				FVector2D TextureSize = Texture.Area.GetSize();
				float TextureSurface = TextureSize.X * TextureSize.Y;

				// Find best area to insert this texture in (determined by tightest fit)
				for (int32 AreaIndex = 0; AreaIndex < UnusedAreas.Num(); ++AreaIndex)
				{
					const FUnusedArea& UnusedArea = UnusedAreas[AreaIndex];
					if (UnusedArea.GetSize() >= TextureSize)
					{
						const float Remainder = UnusedArea.GetArea() - TextureSurface;
						if (Remainder < RemainingArea && Remainder >= 0)
						{
							BestAreaIndex = AreaIndex;
							RemainingArea = Remainder;
						}
					}
				}

				// Insert the texture in case we found an appropriate area
				if (BestAreaIndex != -1)
				{
					FUnusedArea& UnusedArea = UnusedAreas[BestAreaIndex];
					FVector2D UnusedSize = UnusedArea.GetSize();

					// Push back texture
					FWeightedTexture WeightedTexture;
					WeightedTexture.Area = FTextureArea(UnusedArea.Min, UnusedArea.Min + TextureSize);
					WeightedTexture.TextureIndex = Texture.TextureIndex;
					InsertedTextures.Add(WeightedTexture);

					// Generate two new resulting unused areas from splitting up the result
					/*
						___________
						|	  |   |
						|	  | V |
						|_____|   |
						|  H  |   |
						|_____|___|
					*/
					FUnusedArea HorizontalArea, VerticalArea;
					HorizontalArea.Min.X = UnusedArea.Min.X;
					HorizontalArea.Min.Y = UnusedArea.Min.Y + TextureSize.Y;
					HorizontalArea.Max.X = HorizontalArea.Min.X + TextureSize.X;
					HorizontalArea.Max.Y = HorizontalArea.Min.Y + (UnusedSize.Y - TextureSize.Y);

					VerticalArea.Min.X = UnusedArea.Min.X + TextureSize.X;
					VerticalArea.Min.Y = UnusedArea.Min.Y;
					VerticalArea.Max.X = VerticalArea.Min.X + (UnusedSize.X - TextureSize.X);
					VerticalArea.Max.Y = UnusedSize.Y;

					// Append valid new areas to list (replace original one with either one of the new ones)
					const bool bValidHorizontal = HorizontalArea.GetArea() > 0.0f;
					const bool bValidVertical = VerticalArea.GetArea() > 0.0f;
					if (bValidVertical && bValidHorizontal)
					{
						UnusedAreas[BestAreaIndex] = HorizontalArea;
						UnusedAreas.Add(VerticalArea);
					}
					else if (bValidVertical)
					{
						UnusedAreas[BestAreaIndex] = VerticalArea;
					}
					else if (bValidHorizontal)
					{
						UnusedAreas[BestAreaIndex] = HorizontalArea;
					}
					else
					{
						// Make sure we remove the area entry
						UnusedAreas.RemoveAtSwap(BestAreaIndex);
					}
				}
				else
				{
					bSuccess = false;
					break;
				}
			}

			// This means we failed to find a fit, in this case we resize the textures and try again until we find one
			if (bSuccess == false)
			{
				for (FWeightedTexture& Texture : WeightedTextures)
				{
					Texture.Area.Max *= .99f;
				}
			}
		} while (!bSuccess);

		// Now generate boxes
		OutGeneratedBoxes.Empty(InTexureSize.Num());
		OutGeneratedBoxes.AddZeroed(InTexureSize.Num());

		// Generate boxes according to the inserted textures
		for (const FWeightedTexture& Texture : InsertedTextures)
		{
			FBox2D& Box = OutGeneratedBoxes[Texture.TextureIndex];
			Box = Texture.Area;
		}
	}

	UTexture2D* CreateCompositeTexture(UObject* WorldContextObject, const FIntPoint& Size, bool bNormal,
		const TArray<UTexture*>* Textures, const TArray<FBox2D>* Boxes)
	{
		if (Size.X == 0 || Size.Y == 0 || !Textures || !Boxes || Textures->Num() != Boxes->Num() || Textures->Num() < 0)
			return nullptr;

		UTexture2D* FirstTexture = Cast<UTexture2D>((*Textures)[0]);
		check(FirstTexture);

		UTexture2D* DestinationTexture = UTexture2D::CreateTransient(Size.X, Size.Y, FirstTexture->GetPixelFormat());
		check(DestinationTexture);

		DestinationTexture->SRGB = bNormal ? 0 : 1;

		DestinationTexture->UpdateResource();

		for (int32 i = 0; i < Textures->Num(); i++)
		{
			const FBox2D& Box = (*Boxes)[i];
			UTexture2D* SourceTexture = Cast<UTexture2D>((*Textures)[i]);
			check(SourceTexture);

			if (SourceTexture->GetPixelFormat() != DestinationTexture->GetPixelFormat())
				continue;

			ENQUEUE_RENDER_COMMAND(InitCommand)(
				[SourceTexture, DestinationTexture, Box](FRHICommandListImmediate& RHICmdList)
			{
				int32 SizeX = SourceTexture->GetSizeX();
				int32 SizeY = SourceTexture->GetSizeY();

				FResolveRect SrcRect(0, 0, SizeX, SizeY);
				FResolveRect DestRect(Box.Min.X, Box.Min.Y, Box.Min.X + SizeX, Box.Min.Y + SizeY);

				FResolveParams ResolveParams(
					SrcRect,
					CubeFace_PosX,
					0,
					0,
					0,
					DestRect
				);

				RHICmdList.CopyToResolveTarget(SourceTexture->Resource->TextureRHI, DestinationTexture->Resource->TextureRHI, ResolveParams);
				/*
				FOpenGLDynamicRHI::RHICopyToResolveTarget这个函数有点问题，他仅支持同位置的拷贝，因此需要修改源码。
				修改：FOpenGL::CopyImageSubData这个函数调用时，GLint DstX, GLint DstY要传对，源码里传的是SrcX和SrcY。
				*/
			});
		}

		FlushRenderingCommands();

		return DestinationTexture;
	}
}

const int MaterialPropertyCount = 2; // BaseColor, Normap
const EMaterialProperty MaterialProperties[MaterialPropertyCount] = { MP_BaseColor, MP_Normal, };
const FName MaterialPropertyTextureNames[MaterialPropertyCount] = { TEXT("MainTexture"), TEXT("NormalMap") };
const FIntPoint MaterialPropertyTextureSize[MaterialPropertyCount] = { FIntPoint(1024, 1024), FIntPoint(1024, 1024), };
const bool MaterialPropertyIsNormal[MaterialPropertyCount] = { false, true, };

/*
目前存在的限制：
1）每个Tile纹理的格式必须一致
2）每张Atlas纹理的尺寸必须一致（因为UV只有一套）
*/

void FCustomSkeletalMeshMerge::MergeMaterial()
{
	typedef TPair<int32, int32> FMeshSectionKey; // MeshIdx, MtlIdx
	TArray<UMaterialInterface*> MaterialList; // 材质队列
	TMap<FMeshSectionKey, int32> MeshSectionToMaterialList; // 通过MeshSection查找材质
	TArray<FVector2D> TextureSize; // 材质的权重

	// 收集所有材质
	for (int32 MeshIdx = 0; MeshIdx < SrcMeshList.Num(); MeshIdx++)
	{
		USkeletalMesh* SrcMesh = SrcMeshList[MeshIdx];
		for (int32 MtlIdx = 0; MtlIdx < SrcMesh->Materials.Num(); MtlIdx++)
		{
			FSkeletalMaterial& Material = SrcMesh->Materials[MtlIdx];
			MaterialList.Add(Material.MaterialInterface);
			MeshSectionToMaterialList.Add(FMeshSectionKey(MeshIdx, MtlIdx), MaterialList.Num() - 1);

			// 计算所占比例（根据纹理大小
			UTexture* MainTexture = nullptr;
			Material.MaterialInterface->GetTextureParameterValue(MaterialPropertyTextureNames[0], MainTexture);
			UTexture2D* MainTexture2D = Cast<UTexture2D>(MainTexture);
			check(MainTexture2D);
			TextureSize.Add(FVector2D(MainTexture2D->GetSizeX(), MainTexture2D->GetSizeY()));

			TArray<UTexture*> MaterialTextures;
			Material.MaterialInterface->GetUsedTextures(MaterialTextures, EMaterialQualityLevel::Num, true, GMaxRHIFeatureLevel, true);

			// Force load materials used by the current material
			for (UTexture* Texture : MaterialTextures)
			{
				if (Texture != NULL)
				{
					UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
					if (Texture2D)
					{
						Texture2D->SetForceMipLevelsToBeResident(30.0f);
						Texture2D->WaitForStreaming();
					}
				}
			}
		}
	}

	// 分配纹理位置
	TArray<FBox2D> UVBoxes;
	GeneratedBinnedTextureSquares(MaterialPropertyTextureSize[0], TextureSize, UVBoxes);

	// 创建材质
	MergedMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, nullptr);
	checkf(MergedMaterial, TEXT("Failed to create material"));

	// 对每个MaterialProperty生成纹理
	for (int32 PropertyIndex = 0; PropertyIndex < MaterialPropertyCount; PropertyIndex++)
	{
		// 收集纹理
		TArray<UTexture*> Textures;
		Textures.AddDefaulted(MaterialList.Num());
		for (int32 MaterialIndex = 0; MaterialIndex < MaterialList.Num(); MaterialIndex++)
		{
			UMaterialInterface* Material = MaterialList[MaterialIndex];
			Material->GetTextureParameterValue(MaterialPropertyTextureNames[PropertyIndex], Textures[MaterialIndex]);
		}

		// 合并纹理
		UTexture2D* CompositeTexture = CreateCompositeTexture(GEngine->GetWorld(),
			MaterialPropertyTextureSize[PropertyIndex], MaterialPropertyIsNormal[PropertyIndex], &Textures, &UVBoxes);

		MergedMaterial->SetTextureParameterValue(MaterialPropertyTextureNames[PropertyIndex], CompositeTexture);
	}

	// 存储UVTransform，供MeshMerge使用
	UVTransformsPerMesh.AddDefaulted(SrcMeshList.Num());
	for (int32 MeshIdx = 0; MeshIdx < SrcMeshList.Num(); MeshIdx++)
	{
		USkeletalMesh* SrcMesh = SrcMeshList[MeshIdx];
		for (int32 MtlIdx = 0; MtlIdx < SrcMesh->Materials.Num(); MtlIdx++)
		{
			int MaterialDataIndex = *MeshSectionToMaterialList.Find(FMeshSectionKey(MeshIdx, MtlIdx));
			const FBox2D& Box = UVBoxes[MaterialDataIndex];
			const FVector2D Pos = Box.Min / MaterialPropertyTextureSize[0];
			const FVector2D Size = Box.GetSize() / MaterialPropertyTextureSize[0];
			FTransform Transform = FTransform(FQuat::Identity, FVector(Pos.X, Pos.Y, 0), FVector(Size.X, Size.Y, 1));
			UVTransformsPerMesh[MeshIdx].Add(Transform);
		}
	}
}

void FCustomSkeletalMeshMerge::MergeSkeleton(const TArray<FRefPoseOverride>* RefPoseOverrides /* = nullptr */)
{
	// Release the rendering resources.

	MergeMesh->ReleaseResources();
	MergeMesh->ReleaseResourcesFence.Wait();

	// Build the reference skeleton & sockets.

	BuildReferenceSkeleton(SrcMeshList, NewRefSkeleton, MergeMesh->Skeleton);
	BuildSockets(SrcMeshList);

	// Override the reference bone poses & sockets, if specified.

	if (RefPoseOverrides)
	{
		OverrideReferenceSkeletonPose(*RefPoseOverrides, NewRefSkeleton, MergeMesh->Skeleton);
		OverrideMergedSockets(*RefPoseOverrides);
	}

	// Assign new referencer skeleton.

	MergeMesh->RefSkeleton = NewRefSkeleton;

	// Rebuild inverse ref pose matrices here as some access patterns 
	// may need to access these matrices before FinalizeMesh is called
	// (which would *normally* rebuild the inv ref matrices).
	MergeMesh->RefBasesInvMatrix.Empty();
	MergeMesh->CalculateInvRefMatrices();
}

namespace
{
	TArray<FTransform> GetComponentSpaceTransforms(const FReferenceSkeleton& RefSkeleton)
	{
		TArray<FTransform> ComponentSpaceTransforms;

		ComponentSpaceTransforms.AddUninitialized(RefSkeleton.GetRawBoneNum());

		auto& LocalTransform = RefSkeleton.GetRawRefBonePose();

		check(LocalTransform.Num() == ComponentSpaceTransforms.Num());

		const FTransform* LocalTransformsData = LocalTransform.GetData();
		FTransform* ComponentSpaceData = ComponentSpaceTransforms.GetData();

		ComponentSpaceTransforms[0] = LocalTransform[0];

		for (int32 BoneIndex = 1; BoneIndex < LocalTransform.Num(); BoneIndex++)
		{
			// For all bones below the root, final component-space transform is relative transform * component-space transform of parent.
			const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
			FTransform* ParentSpaceBase = ComponentSpaceData + ParentIndex;
			FPlatformMisc::Prefetch(ParentSpaceBase);

			FTransform* SpaceBase = ComponentSpaceData + BoneIndex;

			FTransform::Multiply(SpaceBase, LocalTransformsData + BoneIndex, ParentSpaceBase);

			SpaceBase->NormalizeRotation();

			checkSlow(SpaceBase->IsRotationNormalized());
			checkSlow(!SpaceBase->ContainsNaN());
		}

		return ComponentSpaceTransforms;
	}
}

bool FCustomSkeletalMeshMerge::FinalizeMesh()
{
	bool Result = true;

	// Find the common maximum number of LODs available in the list of source meshes.

	int32 MaxNumLODs = CalculateLodCount(SrcMeshList);

	if (MaxNumLODs == -1)
	{
		UE_LOG(LogSkeletalMesh, Warning, TEXT("FCustomSkeletalMeshMerge: Invalid source mesh list"));
		return false;
	}

	ReleaseResources(MaxNumLODs);

	// Create a mapping from each input mesh bone to bones in the merged mesh.

	TArray<FTransform> ComponentSpaceTransforms = GetComponentSpaceTransforms(NewRefSkeleton);

	SrcMeshInfo.Empty();
	SrcMeshInfo.AddZeroed(SrcMeshList.Num());

	for (int32 MeshIdx = 0; MeshIdx < SrcMeshList.Num(); MeshIdx++)
	{
		USkeletalMesh* SrcMesh = SrcMeshList[MeshIdx];
		if (SrcMesh)
		{
			if (SrcMesh->bHasVertexColors)
			{
				MergeMesh->bHasVertexColors = true;
#if WITH_EDITORONLY_DATA
				MergeMesh->VertexColorGuid = FGuid::NewGuid();
#endif
			}

			FMergeMeshInfo& MeshInfo = SrcMeshInfo[MeshIdx];
			MeshInfo.SrcToDestRefSkeletonMap.AddUninitialized(SrcMesh->RefSkeleton.GetRawBoneNum());

			FName AttachedBoneName = SrcMeshAttachedBoneNameList[MeshIdx];
			int32 AttachedBoneIndex = NewRefSkeleton.FindBoneIndex(AttachedBoneName);

			// transform vertices
			if (AttachedBoneIndex != INDEX_NONE)
			{
				TArray<FTransform> SrcBones = GetComponentSpaceTransforms(SrcMesh->RefSkeleton);
				FTransform SrcInvTransform = FTransform::Identity;
				if (SrcBones.Num() > 0)
					SrcInvTransform = SrcBones[0].Inverse();
				FTransform BindingTransform = ComponentSpaceTransforms[AttachedBoneIndex];
				VerticesTransformList[MeshIdx] = VerticesTransformList[MeshIdx] * SrcInvTransform * BindingTransform;
			}

			// remap skin
			for (int32 i = 0; i < SrcMesh->RefSkeleton.GetRawBoneNum(); i++)
			{
				int32 DestBoneIndex = AttachedBoneIndex;

				if (DestBoneIndex == INDEX_NONE)
				{
					FName SrcBoneName = SrcMesh->RefSkeleton.GetBoneName(i);
					DestBoneIndex = NewRefSkeleton.FindBoneIndex(SrcBoneName);
				}

				if (DestBoneIndex == INDEX_NONE)
				{
					int32 ParentIndex = SrcMesh->RefSkeleton.GetParentIndex(i);
					for (int32 j = 0; j < 3; j++)
					{
						if (ParentIndex == INDEX_NONE)
							break;

						FName SrcBoneName = SrcMesh->RefSkeleton.GetBoneName(ParentIndex);
						DestBoneIndex = NewRefSkeleton.FindBoneIndex(SrcBoneName);

						if (DestBoneIndex == INDEX_NONE)
							ParentIndex = SrcMesh->RefSkeleton.GetParentIndex(ParentIndex);
						else
							break;
					}
				}

				if (DestBoneIndex == INDEX_NONE)
				{
					// Missing bones shouldn't be possible, but can happen with invalid meshes;
					// map any bone we are missing to the 'root'.

					DestBoneIndex = 0;
				}

				MeshInfo.SrcToDestRefSkeletonMap[i] = DestBoneIndex;
			}
		}
	}

	// If things are going ok so far...
	if (Result)
	{
		// force 16 bit UVs if supported on hardware
		MergeMesh->bUseFullPrecisionUVs = GVertexElementTypeSupport.IsSupported(VET_Half2) ? false : true;

		// Array of per-lod number of UV sets
		TArray<uint32> PerLODNumUVSets;
		TArray<bool> PerLODExtraBoneInfluences;
		PerLODNumUVSets.AddZeroed(MaxNumLODs);
		PerLODExtraBoneInfluences.AddZeroed(MaxNumLODs);

		// Get the number of UV sets for each LOD.
		for (int32 MeshIdx = 0; MeshIdx < SrcMeshList.Num(); MeshIdx++)
		{
			USkeletalMesh* SrcSkelMesh = SrcMeshList[MeshIdx];
			FSkeletalMeshRenderData* SrcResource = SrcSkelMesh->GetResourceForRendering();

			for (int32 LODIdx = 0; LODIdx < MaxNumLODs; LODIdx++)
			{
				if (SrcResource->LODRenderData.IsValidIndex(LODIdx))
				{
					uint32& NumUVSets = PerLODNumUVSets[LODIdx];
					NumUVSets = FMath::Max(NumUVSets, SrcResource->LODRenderData[LODIdx].GetNumTexCoords());

					PerLODExtraBoneInfluences[LODIdx] |= SrcResource->LODRenderData[LODIdx].DoesVertexBufferHaveExtraBoneInfluences();
				}
			}
		}

		// process each LOD for the new merged mesh
		MergeMesh->AllocateResourceForRendering();
		for (int32 LODIdx = 0; LODIdx < MaxNumLODs; LODIdx++)
		{
			if (!MergeMesh->bUseFullPrecisionUVs)
			{
				if (PerLODExtraBoneInfluences[LODIdx])
				{
					GENERATE_LOD_MODEL(TGPUSkinVertexFloat16Uvs, PerLODNumUVSets[LODIdx], true);
				}
				else
				{
					GENERATE_LOD_MODEL(TGPUSkinVertexFloat16Uvs, PerLODNumUVSets[LODIdx], false);
				}
			}
			else
			{
				if (PerLODExtraBoneInfluences[LODIdx])
				{
					GENERATE_LOD_MODEL(TGPUSkinVertexFloat32Uvs, PerLODNumUVSets[LODIdx], true);
				}
				else
				{
					GENERATE_LOD_MODEL(TGPUSkinVertexFloat32Uvs, PerLODNumUVSets[LODIdx], false);
				}
			}
		}
		// update the merge skel mesh entries
		if (!ProcessMergeMesh())
		{
			Result = false;
		}

		// Reinitialize the mesh's render resources.
		MergeMesh->InitResources();
	}

	return Result;
}

/**
* Merge a bonemap with an existing bonemap and keep track of remapping
* (a bonemap is a list of indices of bones in the USkeletalMesh::RefSkeleton array)
* @param MergedBoneMap - out merged bonemap
* @param BoneMapToMergedBoneMap - out of mapping from original bonemap to new merged bonemap
* @param BoneMap - input bonemap to merge
*/
void FCustomSkeletalMeshMerge::MergeBoneMap(TArray<FBoneIndexType>& MergedBoneMap, TArray<FBoneIndexType>& BoneMapToMergedBoneMap, const TArray<FBoneIndexType>& BoneMap)
{
	BoneMapToMergedBoneMap.AddUninitialized(BoneMap.Num());
	for (int32 IdxB = 0; IdxB < BoneMap.Num(); IdxB++)
	{
		BoneMapToMergedBoneMap[IdxB] = MergedBoneMap.AddUnique(BoneMap[IdxB]);
	}
}

static void BoneMapToNewRefSkel(const TArray<FBoneIndexType>& InBoneMap, const TArray<int32>& SrcToDestRefSkeletonMap, TArray<FBoneIndexType>& OutBoneMap)
{
	OutBoneMap.Empty();
	OutBoneMap.AddUninitialized(InBoneMap.Num());

	for (int32 i = 0; i < InBoneMap.Num(); i++)
	{
		check(InBoneMap[i] < SrcToDestRefSkeletonMap.Num());
		OutBoneMap[i] = SrcToDestRefSkeletonMap[InBoneMap[i]];
	}
}

/**
* Generate the list of sections that need to be created along with info needed to merge sections
* @param NewSectionArray - out array to populate
* @param LODIdx - current LOD to process
*/
void FCustomSkeletalMeshMerge::GenerateNewSectionArray(TArray<FNewSectionInfo>& NewSectionArray, int32 LODIdx)
{
	const int32 MaxGPUSkinBones = GetFeatureLevelMaxNumberOfBones(GMaxRHIFeatureLevel);

	NewSectionArray.Empty();
	for (int32 MeshIdx = 0; MeshIdx < SrcMeshList.Num(); MeshIdx++)
	{
		// source mesh
		USkeletalMesh* SrcMesh = SrcMeshList[MeshIdx];
		const FTransform& VerticesTransform = VerticesTransformList[MeshIdx];

		if (SrcMesh)
		{
			FSkeletalMeshRenderData* SrcResource = SrcMesh->GetResourceForRendering();
			int32 SourceLODIdx = FMath::Min(LODIdx, SrcResource->LODRenderData.Num() - 1);
			FSkeletalMeshLODRenderData& SrcLODData = SrcResource->LODRenderData[SourceLODIdx];
			FSkeletalMeshLODInfo& SrcLODInfo = *(SrcMesh->GetLODInfo(SourceLODIdx));

			// iterate over each section of this LOD
			for (int32 SectionIdx = 0; SectionIdx < SrcLODData.RenderSections.Num(); SectionIdx++)
			{
				int32 MaterialId = -1;
				// check for the optional list of material ids corresponding to the list of src meshes
				// if the id is valid (not -1) it is used to find an existing section entry to merge with
				if (ForceSectionMapping.Num() == SrcMeshList.Num() &&
					ForceSectionMapping.IsValidIndex(MeshIdx) &&
					ForceSectionMapping[MeshIdx].SectionIDs.IsValidIndex(SectionIdx))
				{
					MaterialId = ForceSectionMapping[MeshIdx].SectionIDs[SectionIdx];
				}

				FSkelMeshRenderSection& Section = SrcLODData.RenderSections[SectionIdx];

				// Convert Chunk.BoneMap from src to dest bone indices
				TArray<FBoneIndexType> DestChunkBoneMap;
				BoneMapToNewRefSkel(Section.BoneMap, SrcMeshInfo[MeshIdx].SrcToDestRefSkeletonMap, DestChunkBoneMap);

				// get the material for this section
				int32 MaterialIndex = Section.MaterialIndex;
				// use the remapping of material indices for all LODs besides the base LOD 
				if (LODIdx > 0 &&
					SrcLODInfo.LODMaterialMap.IsValidIndex(Section.MaterialIndex))
				{
					MaterialIndex = FMath::Clamp<int32>(SrcLODInfo.LODMaterialMap[Section.MaterialIndex], 0, SrcMesh->Materials.Num());
				}
				UMaterialInterface* Material = SrcMesh->Materials[MaterialIndex].MaterialInterface;

				// see if there is an existing entry in the array of new sections that matches its material
				// if there is a match then the source section can be added to its list of sections to merge 
				int32 FoundIdx = INDEX_NONE;
				for (int32 Idx = 0; Idx < NewSectionArray.Num(); Idx++)
				{
					FNewSectionInfo& NewSectionInfo = NewSectionArray[Idx];
					// check for a matching material or a matching material index id if it is valid
					/* hack
					if ((MaterialId == -1 && Material == NewSectionInfo.Material) ||
						(MaterialId != -1 && MaterialId == NewSectionInfo.MaterialId))
						*/
					{
						check(NewSectionInfo.MergeSections.Num());

						// merge the bonemap from the source section with the existing merged bonemap
						TArray<FBoneIndexType> TempMergedBoneMap(NewSectionInfo.MergedBoneMap);
						TArray<FBoneIndexType> TempBoneMapToMergedBoneMap;
						MergeBoneMap(TempMergedBoneMap, TempBoneMapToMergedBoneMap, DestChunkBoneMap);

						// check to see if the newly merged bonemap is still within the bone limit for GPU skinning
						if (TempMergedBoneMap.Num() <= MaxGPUSkinBones)
						{
							TArray<FTransform> SrcUVTransform;
							if (MeshIdx < UVTransformsPerMesh.Num())
							{
								SrcUVTransform = UVTransformsPerMesh[MeshIdx];
							}

							// add the source section as a new merge entry
							FMergeSectionInfo& MergeSectionInfo = *new(NewSectionInfo.MergeSections) FMergeSectionInfo(
								SrcMesh,
								&SrcLODData.RenderSections[SectionIdx],
								SrcUVTransform,
								VerticesTransform
							);
							// keep track of remapping for the existing chunk's bonemap 
							// so that the bone matrix indices can be updated for the vertices
							MergeSectionInfo.BoneMapToMergedBoneMap = TempBoneMapToMergedBoneMap;

							// use the updated bonemap for this new section
							NewSectionInfo.MergedBoneMap = TempMergedBoneMap;

							// keep track of the entry that was found
							FoundIdx = Idx;
							break;
						}
					}
				}

				// new section entries will be created if the material for the source section was not found 
				// or merging it with an existing entry would go over the bone limit for GPU skinning
				if (FoundIdx == INDEX_NONE)
				{
					// create a new section entry
					const FMeshUVChannelInfo& UVChannelData = SrcMesh->Materials[MaterialIndex].UVChannelData;
					/* hack
					FNewSectionInfo& NewSectionInfo = *new(NewSectionArray) FNewSectionInfo(Material, MaterialId, UVChannelData);
					*/
					FNewSectionInfo& NewSectionInfo = *new(NewSectionArray) FNewSectionInfo(MergedMaterial, MaterialId, UVChannelData);
					// initialize the merged bonemap to simply use the original chunk bonemap
					NewSectionInfo.MergedBoneMap = DestChunkBoneMap;

					TArray<FTransform> SrcUVTransform;
					if (MeshIdx < UVTransformsPerMesh.Num())
					{
						SrcUVTransform = UVTransformsPerMesh[MeshIdx];
					}
					// add a new merge section entry
					FMergeSectionInfo& MergeSectionInfo = *new(NewSectionInfo.MergeSections) FMergeSectionInfo(
						SrcMesh,
						&SrcLODData.RenderSections[SectionIdx],
						SrcUVTransform,
						VerticesTransform);
					// since merged bonemap == chunk.bonemap then remapping is just pass-through
					MergeSectionInfo.BoneMapToMergedBoneMap.Empty(DestChunkBoneMap.Num());
					for (int32 i = 0; i < DestChunkBoneMap.Num(); i++)
					{
						MergeSectionInfo.BoneMapToMergedBoneMap.Add((FBoneIndexType)i);
					}
				}
			}
		}
	}
}

template<typename VertexDataType>
void FCustomSkeletalMeshMerge::CopyVertexFromSource(VertexDataType& DestVert, const FSkeletalMeshLODRenderData& SrcLODData, int32 SourceVertIdx, const FMergeSectionInfo& MergeSectionInfo)
{
	DestVert.Position = SrcLODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(SourceVertIdx);
	DestVert.Position = MergeSectionInfo.VerticesTransform.TransformFVector4(DestVert.Position);
	DestVert.TangentX = SrcLODData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentX(SourceVertIdx);
	DestVert.TangentZ = SrcLODData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(SourceVertIdx);

	// Copy all UVs that are available
	uint32 LODNumTexCoords = SrcLODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords();
	for (uint32 UVIndex = 0; UVIndex < LODNumTexCoords && UVIndex < VertexDataType::NumTexCoords; ++UVIndex)
	{
		FVector2D UVs = SrcLODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV_Typed<VertexDataType::StaticMeshVertexUVType>(SourceVertIdx, UVIndex);
		if (UVIndex < (uint32)MergeSectionInfo.UVTransforms.Num())
		{
			FVector Transformed = MergeSectionInfo.UVTransforms[UVIndex].TransformPosition(FVector(UVs, 1.f));
			UVs = FVector2D(Transformed.X, Transformed.Y);
		}
		DestVert.UVs[UVIndex] = UVs;
	}
}

template<typename SkinWeightType, bool bHasExtraBoneInfluences>
void FCustomSkeletalMeshMerge::CopyWeightFromSource(SkinWeightType& DestWeight, const FSkeletalMeshLODRenderData& SrcLODData, int32 SourceVertIdx, const FMergeSectionInfo& MergeSectionInfo)
{
	const TSkinWeightInfo<bHasExtraBoneInfluences>* SrcSkinWeights = SrcLODData.SkinWeightVertexBuffer.GetSkinWeightPtr<bHasExtraBoneInfluences>(SourceVertIdx);

	// if source doesn't have extra influence, we have to clear the buffer
	FMemory::Memzero(DestWeight.InfluenceBones);
	FMemory::Memzero(DestWeight.InfluenceWeights);

	FMemory::Memcpy(DestWeight.InfluenceBones, SrcSkinWeights->InfluenceBones, sizeof(SrcSkinWeights->InfluenceBones));
	FMemory::Memcpy(DestWeight.InfluenceWeights, SrcSkinWeights->InfluenceWeights, sizeof(SrcSkinWeights->InfluenceWeights));
}

/**
* Creates a new LOD model and adds the new merged sections to it. Modifies the MergedMesh.
* @param LODIdx - current LOD to process
*/
template<typename VertexDataType, typename SkinWeightType>
void FCustomSkeletalMeshMerge::GenerateLODModel(int32 LODIdx)
{
	// add the new LOD model entry
	FSkeletalMeshRenderData* MergeResource = MergeMesh->GetResourceForRendering();
	check(MergeResource);

	FSkeletalMeshLODRenderData& MergeLODData = *new FSkeletalMeshLODRenderData;
	MergeResource->LODRenderData.Add(&MergeLODData);
	// add the new LOD info entry
	FSkeletalMeshLODInfo& MergeLODInfo = MergeMesh->AddLODInfo();
	MergeLODInfo.ScreenSize = MergeLODInfo.LODHysteresis = MAX_FLT;

	// generate an array with info about new sections that need to be created
	TArray<FNewSectionInfo> NewSectionArray;
	GenerateNewSectionArray(NewSectionArray, LODIdx);

	uint32 MaxIndex = 0;

	// merged vertex buffer
	TArray< VertexDataType > MergedVertexBuffer;
	// merged skin weight buffer
	TArray< SkinWeightType > MergedSkinWeightBuffer;
	// merged vertex color buffer
	TArray< FColor > MergedColorBuffer;
	// merged index buffer
	TArray<uint32> MergedIndexBuffer;

	// The total number of UV sets for this LOD model
	uint32 TotalNumUVs = 0;

	// true if any extra bone influence exists
	bool bSourceHasExtraBoneInfluences = false;

	for (int32 CreateIdx = 0; CreateIdx < NewSectionArray.Num(); CreateIdx++)
	{
		FNewSectionInfo& NewSectionInfo = NewSectionArray[CreateIdx];

		// ActiveBoneIndices contains all the bones used by the verts from all the sections of this LOD model
		// Add the bones used by this new section
		for (int32 Idx = 0; Idx < NewSectionInfo.MergedBoneMap.Num(); Idx++)
		{
			MergeLODData.ActiveBoneIndices.AddUnique(NewSectionInfo.MergedBoneMap[Idx]);
		}

		// add the new section entry
		FSkelMeshRenderSection& Section = *new(MergeLODData.RenderSections) FSkelMeshRenderSection;

		// set the new bonemap from the merged sections
		// these are the bones that will be used by this new section
		Section.BoneMap = NewSectionInfo.MergedBoneMap;

		// init vert totals
		Section.NumVertices = 0;

		// keep track of the current base vertex for this section in the merged vertex buffer
		Section.BaseVertexIndex = MergedVertexBuffer.Num();


		// find existing material index
		check(MergeMesh->Materials.Num() == MaterialIds.Num());
		int32 MatIndex;
		if (NewSectionInfo.MaterialId == -1)
		{
			MatIndex = MergeMesh->Materials.Find(NewSectionInfo.Material);
		}
		else
		{
			MatIndex = MaterialIds.Find(NewSectionInfo.MaterialId);
		}

		// if it doesn't exist, make new entry
		if (MatIndex == INDEX_NONE)
		{
			FSkeletalMaterial SkeletalMaterial(NewSectionInfo.Material, true);
			SkeletalMaterial.UVChannelData = NewSectionInfo.UVChannelData;
			MergeMesh->Materials.Add(SkeletalMaterial);
			MaterialIds.Add(NewSectionInfo.MaterialId);
			Section.MaterialIndex = MergeMesh->Materials.Num() - 1;
		}
		else
		{
			Section.MaterialIndex = MatIndex;
		}

		// init tri totals
		Section.NumTriangles = 0;
		// keep track of the current base index for this section in the merged index buffer
		Section.BaseIndex = MergedIndexBuffer.Num();

		FMeshUVChannelInfo& MergedUVData = MergeMesh->Materials[Section.MaterialIndex].UVChannelData;

		// iterate over all of the sections that need to be merged together
		for (int32 MergeIdx = 0; MergeIdx < NewSectionInfo.MergeSections.Num(); MergeIdx++)
		{
			FMergeSectionInfo& MergeSectionInfo = NewSectionInfo.MergeSections[MergeIdx];
			int32 SourceLODIdx = FMath::Min(LODIdx, MergeSectionInfo.SkelMesh->GetResourceForRendering()->LODRenderData.Num() - 1);

			// Take the max UV density for each UVChannel between all sections that are being merged.
			{
				const int32 NewSectionMatId = MergeSectionInfo.Section->MaterialIndex;
				const FMeshUVChannelInfo& NewSectionUVData = MergeSectionInfo.SkelMesh->Materials[NewSectionMatId].UVChannelData;

				for (int32 i = 0; i < MAX_TEXCOORDS; i++)
				{
					const float NewSectionUVDensity = NewSectionUVData.LocalUVDensities[i];
					float& UVDensity = MergedUVData.LocalUVDensities[i];

					UVDensity = FMath::Max(UVDensity, NewSectionUVDensity);
				}
			}

			// get the source skel LOD info from this merge entry
			const FSkeletalMeshLODInfo& SrcLODInfo = *(MergeSectionInfo.SkelMesh->GetLODInfo(SourceLODIdx));

			// keep track of the lowest LOD displayfactor and hysteresis
			MergeLODInfo.ScreenSize.Default = FMath::Min<float>(MergeLODInfo.ScreenSize.Default, SrcLODInfo.ScreenSize.Default);
#if WITH_EDITORONLY_DATA
			for (const TTuple<FName, float>& PerPlatform : SrcLODInfo.ScreenSize.PerPlatform)
			{
				float* Value = MergeLODInfo.ScreenSize.PerPlatform.Find(PerPlatform.Key);
				if (Value)
				{
					*Value = FMath::Min<float>(PerPlatform.Value, *Value);
				}
				else
				{
					MergeLODInfo.ScreenSize.PerPlatform.Add(PerPlatform.Key, PerPlatform.Value);
				}
			}
#endif
			MergeLODInfo.LODHysteresis = FMath::Min<float>(MergeLODInfo.LODHysteresis, SrcLODInfo.LODHysteresis);

			// get the source skel LOD model from this merge entry
			const FSkeletalMeshLODRenderData& SrcLODData = MergeSectionInfo.SkelMesh->GetResourceForRendering()->LODRenderData[SourceLODIdx];

			// add required bones from this source model entry to the merge model entry
			for (int32 Idx = 0; Idx < SrcLODData.RequiredBones.Num(); Idx++)
			{
				FName SrcLODBoneName = MergeSectionInfo.SkelMesh->RefSkeleton.GetBoneName(SrcLODData.RequiredBones[Idx]);
				int32 MergeBoneIndex = NewRefSkeleton.FindBoneIndex(SrcLODBoneName);

				if (MergeBoneIndex != INDEX_NONE)
				{
					MergeLODData.RequiredBones.AddUnique(MergeBoneIndex);
				}
			}

			// update vert total
			Section.NumVertices += MergeSectionInfo.Section->NumVertices;

			// update total number of vertices 
			int32 NumTotalVertices = MergeSectionInfo.Section->NumVertices;

			// add the vertices from the original source mesh to the merged vertex buffer					
			int32 MaxVertIdx = FMath::Min<int32>(
				MergeSectionInfo.Section->BaseVertexIndex + NumTotalVertices,
				SrcLODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices()
				);

			int32 MaxColorIdx = SrcLODData.StaticVertexBuffers.ColorVertexBuffer.GetNumVertices();

			// keep track of the current base vertex index before adding any new vertices
			// this will be needed to remap the index buffer values to the new range
			int32 CurrentBaseVertexIndex = MergedVertexBuffer.Num();
			const bool bSourceExtraBoneInfluence = SrcLODData.SkinWeightVertexBuffer.HasExtraBoneInfluences();
			for (int32 VertIdx = MergeSectionInfo.Section->BaseVertexIndex; VertIdx < MaxVertIdx; VertIdx++)
			{
				// add the new vertex
				VertexDataType& DestVert = MergedVertexBuffer[MergedVertexBuffer.AddUninitialized()];
				SkinWeightType& DestWeight = MergedSkinWeightBuffer[MergedSkinWeightBuffer.AddUninitialized()];

				CopyVertexFromSource<VertexDataType>(DestVert, SrcLODData, VertIdx, MergeSectionInfo);

				bSourceHasExtraBoneInfluences |= bSourceExtraBoneInfluence;
				if (bSourceExtraBoneInfluence)
				{
					CopyWeightFromSource<SkinWeightType, true>(DestWeight, SrcLODData, VertIdx, MergeSectionInfo);
				}
				else
				{
					CopyWeightFromSource<SkinWeightType, false>(DestWeight, SrcLODData, VertIdx, MergeSectionInfo);
				}

				// if the mesh uses vertex colors, copy the source color if possible or default to white
				if (MergeMesh->bHasVertexColors)
				{
					if (VertIdx < MaxColorIdx)
					{
						const FColor& SrcColor = SrcLODData.StaticVertexBuffers.ColorVertexBuffer.VertexColor(VertIdx);
						MergedColorBuffer.Add(SrcColor);
					}
					else
					{
						const FColor ColorWhite(255, 255, 255);
						MergedColorBuffer.Add(ColorWhite);
					}
				}

				uint32 LODNumTexCoords = SrcLODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords();
				if (TotalNumUVs < LODNumTexCoords)
				{
					TotalNumUVs = LODNumTexCoords;
				}

				// remap the bone index used by this vertex to match the mergedbonemap 
				for (int32 Idx = 0; Idx < SkinWeightType::NumInfluences; Idx++)
				{
					if (DestWeight.InfluenceWeights[Idx] > 0)
					{
						checkSlow(MergeSectionInfo.BoneMapToMergedBoneMap.IsValidIndex(DestWeight.InfluenceBones[Idx]));
						DestWeight.InfluenceBones[Idx] = (uint8)MergeSectionInfo.BoneMapToMergedBoneMap[DestWeight.InfluenceBones[Idx]];
					}
				}
			}

			// update total number of triangles
			Section.NumTriangles += MergeSectionInfo.Section->NumTriangles;

			// add the indices from the original source mesh to the merged index buffer					
			int32 MaxIndexIdx = FMath::Min<int32>(
				MergeSectionInfo.Section->BaseIndex + MergeSectionInfo.Section->NumTriangles * 3,
				SrcLODData.MultiSizeIndexContainer.GetIndexBuffer()->Num()
				);
			for (int32 IndexIdx = MergeSectionInfo.Section->BaseIndex; IndexIdx < MaxIndexIdx; IndexIdx++)
			{
				uint32 SrcIndex = SrcLODData.MultiSizeIndexContainer.GetIndexBuffer()->Get(IndexIdx);

				// add offset to each index to match the new entries in the merged vertex buffer
				checkSlow(SrcIndex >= MergeSectionInfo.Section->BaseVertexIndex);
				uint32 DstIndex = SrcIndex - MergeSectionInfo.Section->BaseVertexIndex + CurrentBaseVertexIndex;
				checkSlow(DstIndex < (uint32)MergedVertexBuffer.Num());

				// add the new index to the merged vertex buffer
				MergedIndexBuffer.Add(DstIndex);
				if (MaxIndex < DstIndex)
				{
					MaxIndex = DstIndex;
				}

			}

			{
				if (MergeSectionInfo.Section->DuplicatedVerticesBuffer.bHasOverlappingVertices)
				{
					if (Section.DuplicatedVerticesBuffer.bHasOverlappingVertices)
					{
						// Merge
						int32 StartIndex = Section.DuplicatedVerticesBuffer.DupVertData.Num();
						int32 StartVertex = Section.DuplicatedVerticesBuffer.DupVertIndexData.Num();
						Section.DuplicatedVerticesBuffer.DupVertData.ResizeBuffer(StartIndex + MergeSectionInfo.Section->DuplicatedVerticesBuffer.DupVertData.Num());
						Section.DuplicatedVerticesBuffer.DupVertIndexData.ResizeBuffer(Section.NumVertices);

						uint8* VertData = Section.DuplicatedVerticesBuffer.DupVertData.GetDataPointer();
						uint8* IndexData = Section.DuplicatedVerticesBuffer.DupVertIndexData.GetDataPointer();
						for (int32 i = StartIndex; i < Section.DuplicatedVerticesBuffer.DupVertData.Num(); ++i)
						{
							*((uint32*)(VertData + i * sizeof(uint32))) += CurrentBaseVertexIndex - MergeSectionInfo.Section->BaseVertexIndex;
						}
						for (uint32 i = StartVertex; i < Section.NumVertices; ++i)
						{
							((FIndexLengthPair*)(IndexData + i * sizeof(FIndexLengthPair)))->Index += StartIndex;
						}
					}
					else
					{
						Section.DuplicatedVerticesBuffer.DupVertData = MergeSectionInfo.Section->DuplicatedVerticesBuffer.DupVertData;
						Section.DuplicatedVerticesBuffer.DupVertIndexData = MergeSectionInfo.Section->DuplicatedVerticesBuffer.DupVertIndexData;
						uint8* VertData = Section.DuplicatedVerticesBuffer.DupVertData.GetDataPointer();
						for (uint32 i = 0; i < MergeSectionInfo.Section->NumVertices; ++i)
						{
							*((uint32*)(VertData + i * sizeof(uint32))) += CurrentBaseVertexIndex - MergeSectionInfo.Section->BaseVertexIndex;
						}
					}
					Section.DuplicatedVerticesBuffer.bHasOverlappingVertices = true;
				}
				else
				{
					Section.DuplicatedVerticesBuffer.DupVertData.ResizeBuffer(1);
					Section.DuplicatedVerticesBuffer.DupVertIndexData.ResizeBuffer(Section.NumVertices);

					uint8* VertData = Section.DuplicatedVerticesBuffer.DupVertData.GetDataPointer();
					uint8* IndexData = Section.DuplicatedVerticesBuffer.DupVertIndexData.GetDataPointer();

					FMemory::Memzero(IndexData, Section.NumVertices * sizeof(FIndexLengthPair));
					FMemory::Memzero(VertData, sizeof(uint32));
				}
			}
		}
	}

	const bool bNeedsCPUAccess = (MeshBufferAccess == EMeshBufferAccess::ForceCPUAndGPU) ||
		MergeResource->RequiresCPUSkinning(GMaxRHIFeatureLevel);

	// sort required bone array in strictly increasing order
	MergeLODData.RequiredBones.Sort();
	MergeMesh->RefSkeleton.EnsureParentsExistAndSort(MergeLODData.ActiveBoneIndices);

	// copy the new vertices and indices to the vertex buffer for the new model
	MergeLODData.StaticVertexBuffers.StaticMeshVertexBuffer.SetUseFullPrecisionUVs(MergeMesh->bUseFullPrecisionUVs);

	MergeLODData.StaticVertexBuffers.PositionVertexBuffer.Init(MergedVertexBuffer.Num(), bNeedsCPUAccess);
	MergeLODData.StaticVertexBuffers.StaticMeshVertexBuffer.Init(MergedVertexBuffer.Num(), TotalNumUVs, bNeedsCPUAccess);

	for (int i = 0; i < MergedVertexBuffer.Num(); i++)
	{
		MergeLODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i) = MergedVertexBuffer[i].Position;
		MergeLODData.StaticVertexBuffers.StaticMeshVertexBuffer.SetVertexTangents(i, MergedVertexBuffer[i].TangentX.ToFVector(), MergedVertexBuffer[i].GetTangentY(), MergedVertexBuffer[i].TangentZ.ToFVector());
		for (uint32 j = 0; j < TotalNumUVs; j++)
		{
			MergeLODData.StaticVertexBuffers.StaticMeshVertexBuffer.SetVertexUV(i, j, MergedVertexBuffer[i].UVs[j]);
		}
	}

	MergeLODData.SkinWeightVertexBuffer.SetHasExtraBoneInfluences(bSourceHasExtraBoneInfluences);
	MergeLODData.SkinWeightVertexBuffer.SetNeedsCPUAccess(bNeedsCPUAccess);

	// copy vertex resource arrays
	MergeLODData.SkinWeightVertexBuffer = MergedSkinWeightBuffer;

	if (MergeMesh->bHasVertexColors)
	{
		MergeLODData.StaticVertexBuffers.ColorVertexBuffer.InitFromColorArray(MergedColorBuffer);
	}


	const uint8 DataTypeSize = (MaxIndex < MAX_uint16) ? sizeof(uint16) : sizeof(uint32);
	MergeLODData.MultiSizeIndexContainer.RebuildIndexBuffer(DataTypeSize, MergedIndexBuffer);
}

/**
* (Re)initialize and merge skeletal mesh info from the list of source meshes to the merge mesh
* @return true if succeeded
*/
bool FCustomSkeletalMeshMerge::ProcessMergeMesh()
{
	bool Result = true;

	// copy settings and bone info from src meshes
	bool bNeedsInit = true;

	MergeMesh->SkelMirrorTable.Empty();

	for (int32 MeshIdx = 0; MeshIdx < SrcMeshList.Num(); MeshIdx++)
	{
		USkeletalMesh* SrcMesh = SrcMeshList[MeshIdx];
		if (SrcMesh)
		{
			if (bNeedsInit)
			{
				// initialize the merged mesh with the first src mesh entry used
				MergeMesh->SetImportedBounds(SrcMesh->GetImportedBounds());

				MergeMesh->SkelMirrorAxis = SrcMesh->SkelMirrorAxis;
				MergeMesh->SkelMirrorFlipAxis = SrcMesh->SkelMirrorFlipAxis;

				// only initialize once
				bNeedsInit = false;
			}
			else
			{
				// add bounds
				MergeMesh->SetImportedBounds(MergeMesh->GetImportedBounds() + SrcMesh->GetImportedBounds());
			}
		}
	}

	// Rebuild inverse ref pose matrices.
	MergeMesh->RefBasesInvMatrix.Empty();
	MergeMesh->CalculateInvRefMatrices();

	return Result;
}

int32 FCustomSkeletalMeshMerge::CalculateLodCount(const TArray<USkeletalMesh*>& SourceMeshList) const
{
	int32 LodCount = INT_MAX;

	for (int32 i = 0, MeshCount = SourceMeshList.Num(); i < MeshCount; ++i)
	{
		USkeletalMesh* SourceMesh = SourceMeshList[i];

		if (SourceMesh)
		{
			LodCount = FMath::Min<int32>(LodCount, SourceMesh->GetLODNum());
		}
	}

	if (LodCount == INT_MAX)
	{
		return -1;
	}

	// Decrease the number of LODs we are going to make based on StripTopLODs.
	// But, make sure there is at least one.

	LodCount -= StripTopLODs;
	LodCount = FMath::Max(LodCount, 1);

	return LodCount;
}

void FCustomSkeletalMeshMerge::BuildReferenceSkeleton(const TArray<USkeletalMesh*>& SourceMeshList, FReferenceSkeleton& RefSkeleton, const USkeleton* SkeletonAsset)
{
	RefSkeleton.Empty();

	// Iterate through all the source mesh reference skeletons and compose the merged reference skeleton.

	FReferenceSkeletonModifier RefSkelModifier(RefSkeleton, SkeletonAsset);

	for (int32 MeshIndex = 0; MeshIndex < SourceMeshList.Num(); ++MeshIndex)
	{
		USkeletalMesh* SourceMesh = SourceMeshList[MeshIndex];

		if (!SourceMesh)
		{
			continue;
		}

		// Initialise new RefSkeleton with first mesh.

		if (RefSkeleton.GetRawBoneNum() == 0)
		{
			RefSkeleton = SourceMesh->RefSkeleton;
			continue;
		}

		/*
		// For subsequent meshes, add any missing bones.
		for (int32 i = 1; i < SourceMesh->RefSkeleton.GetRawBoneNum(); ++i)
		{
			FName SourceBoneName = SourceMesh->RefSkeleton.GetBoneName(i);
			int32 TargetBoneIndex = RefSkeleton.FindRawBoneIndex(SourceBoneName);

			// If the source bone is present in the new RefSkeleton, we skip it.

			if (TargetBoneIndex != INDEX_NONE)
			{
				continue;
			}

			// Add the source bone to the RefSkeleton.

			int32 SourceParentIndex = SourceMesh->RefSkeleton.GetParentIndex(i);
			FName SourceParentName = SourceMesh->RefSkeleton.GetBoneName(SourceParentIndex);
			int32 TargetParentIndex = RefSkeleton.FindRawBoneIndex(SourceParentName);

			if (TargetParentIndex == INDEX_NONE)
			{
				continue;
			}

			FMeshBoneInfo MeshBoneInfo = SourceMesh->RefSkeleton.GetRefBoneInfo()[i];
			MeshBoneInfo.ParentIndex = TargetParentIndex;

			RefSkelModifier.Add(MeshBoneInfo, SourceMesh->RefSkeleton.GetRefBonePose()[i]);
		}
		*/
	}
}

void FCustomSkeletalMeshMerge::OverrideReferenceSkeletonPose(const TArray<FRefPoseOverride>& PoseOverrides, FReferenceSkeleton& TargetSkeleton, const USkeleton* SkeletonAsset)
{
	for (int32 i = 0, PoseMax = PoseOverrides.Num(); i < PoseMax; ++i)
	{
		const FRefPoseOverride& PoseOverride = PoseOverrides[i];
		const FReferenceSkeleton& SourceSkeleton = PoseOverride.SkeletalMesh->RefSkeleton;

		FReferenceSkeletonModifier RefSkelModifier(TargetSkeleton, SkeletonAsset);

		for (int32 j = 0, BoneMax = PoseOverride.Overrides.Num(); j < BoneMax; ++j)
		{
			const FName& BoneName = PoseOverride.Overrides[j].BoneName;
			int32 SourceBoneIndex = SourceSkeleton.FindBoneIndex(BoneName);

			if (SourceBoneIndex != INDEX_NONE)
			{
				bool bOverrideBone = (PoseOverride.Overrides[j].OverrideMode == FRefPoseOverride::ChildrenOnly) ? false : true;

				if (bOverrideBone)
				{
					OverrideReferenceBonePose(SourceBoneIndex, SourceSkeleton, RefSkelModifier);
				}

				bool bOverrideChildren = (PoseOverride.Overrides[j].OverrideMode == FRefPoseOverride::BoneOnly) ? false : true;

				if (bOverrideChildren)
				{
					for (int32 ChildBoneIndex = SourceBoneIndex + 1; ChildBoneIndex < SourceSkeleton.GetRawBoneNum(); ++ChildBoneIndex)
					{
						if (SourceSkeleton.BoneIsChildOf(ChildBoneIndex, SourceBoneIndex))
						{
							OverrideReferenceBonePose(ChildBoneIndex, SourceSkeleton, RefSkelModifier);
						}
					}
				}
			}
		}
	}
}

bool FCustomSkeletalMeshMerge::OverrideReferenceBonePose(int32 SourceBoneIndex, const FReferenceSkeleton& SourceSkeleton, FReferenceSkeletonModifier& TargetSkeleton)
{
	FName BoneName = SourceSkeleton.GetBoneName(SourceBoneIndex);
	int32 TargetBoneIndex = TargetSkeleton.GetReferenceSkeleton().FindBoneIndex(BoneName);

	if (TargetBoneIndex != INDEX_NONE)
	{
		const FTransform& SourceBoneTransform = SourceSkeleton.GetRefBonePose()[SourceBoneIndex];
		TargetSkeleton.UpdateRefPoseTransform(TargetBoneIndex, SourceBoneTransform);

		return true;
	}

	return false;
}

void FCustomSkeletalMeshMerge::ReleaseResources(int32 Slack)
{
	FSkeletalMeshRenderData* Resource = MergeMesh->GetResourceForRendering();
	if (Resource)
	{
		Resource->LODRenderData.Empty(Slack);
	}

	MergeMesh->ResetLODInfo();
	MergeMesh->Materials.Empty();
}

bool FCustomSkeletalMeshMerge::AddSocket(const USkeletalMeshSocket* NewSocket, bool bIsSkeletonSocket)
{
	TArray<USkeletalMeshSocket*>& MergeMeshSockets = MergeMesh->GetMeshOnlySocketList();

	// Verify the socket doesn't already exist in the current Mesh list.
	for (USkeletalMeshSocket const * const ExistingSocket : MergeMeshSockets)
	{
		if (ExistingSocket->SocketName == NewSocket->SocketName)
		{
			return false;
		}
	}

	// The Skeleton will only be valid in cases where the passed in mesh already had a skeleton
	// (i.e. an existing mesh was used, or a created mesh was explicitly assigned a skeleton).
	// In either case, we want to avoid adding sockets to the Skeleton (as it is shared), but we
	// still need to check against it to prevent duplication.
	if (bIsSkeletonSocket && MergeMesh->Skeleton)
	{
		for (USkeletalMeshSocket const * const ExistingSocket : MergeMesh->Skeleton->Sockets)
		{
			return false;
		}
	}

	USkeletalMeshSocket* NewSocketDuplicate = CastChecked<USkeletalMeshSocket>(StaticDuplicateObject(NewSocket, MergeMesh));
	MergeMeshSockets.Add(NewSocketDuplicate);

	return true;
}

void FCustomSkeletalMeshMerge::AddSockets(const TArray<USkeletalMeshSocket*>& NewSockets, bool bAreSkeletonSockets)
{
	for (USkeletalMeshSocket* NewSocket : NewSockets)
	{
		AddSocket(NewSocket, bAreSkeletonSockets);
	}
}

void FCustomSkeletalMeshMerge::BuildSockets(const TArray<USkeletalMesh*>& SourceMeshList)
{
	TArray<USkeletalMeshSocket*>& MeshSocketList = MergeMesh->GetMeshOnlySocketList();
	MeshSocketList.Empty();

	// Iterate through the all the source MESH sockets, only adding the new sockets.

	for (USkeletalMesh const * const SourceMesh : SourceMeshList)
	{
		if (SourceMesh)
		{
			const TArray<USkeletalMeshSocket*>& NewMeshSocketList = SourceMesh->GetMeshOnlySocketList();
			AddSockets(NewMeshSocketList, false);
		}
	}

	// Iterate through the all the source SKELETON sockets, only adding the new sockets.

	for (USkeletalMesh const * const SourceMesh : SourceMeshList)
	{
		if (SourceMesh)
		{
			const TArray<USkeletalMeshSocket*>& NewSkeletonSocketList = SourceMesh->Skeleton->Sockets;
			AddSockets(NewSkeletonSocketList, true);
		}
	}
}

void FCustomSkeletalMeshMerge::OverrideSocket(const USkeletalMeshSocket* SourceSocket)
{
	TArray<USkeletalMeshSocket*>& SocketList = MergeMesh->GetMeshOnlySocketList();

	for (int32 i = 0, SocketCount = SocketList.Num(); i < SocketCount; ++i)
	{
		USkeletalMeshSocket* TargetSocket = SocketList[i];

		if (TargetSocket->SocketName == SourceSocket->SocketName)
		{
			TargetSocket->BoneName = SourceSocket->BoneName;
			TargetSocket->RelativeLocation = SourceSocket->RelativeLocation;
			TargetSocket->RelativeRotation = SourceSocket->RelativeRotation;
			TargetSocket->RelativeScale = SourceSocket->RelativeScale;
		}
	}
}

void FCustomSkeletalMeshMerge::OverrideBoneSockets(const FName& BoneName, const TArray<USkeletalMeshSocket*>& SourceSocketList)
{
	for (int32 i = 0, SourceSocketCount = SourceSocketList.Num(); i < SourceSocketCount; ++i)
	{
		const USkeletalMeshSocket* SourceSocket = SourceSocketList[i];

		if (SourceSocket->BoneName == BoneName)
		{
			OverrideSocket(SourceSocket);
		}
	}
}

void FCustomSkeletalMeshMerge::OverrideMergedSockets(const TArray<FRefPoseOverride>& PoseOverrides)
{
	for (int32 i = 0, PoseMax = PoseOverrides.Num(); i < PoseMax; ++i)
	{
		const FRefPoseOverride& PoseOverride = PoseOverrides[i];
		const FReferenceSkeleton& SourceSkeleton = PoseOverride.SkeletalMesh->RefSkeleton;

		const TArray<USkeletalMeshSocket*>& SkeletonSocketList = PoseOverride.SkeletalMesh->Skeleton->Sockets;
		const TArray<USkeletalMeshSocket*>& MeshSocketList = const_cast<USkeletalMesh*>(PoseOverride.SkeletalMesh)->GetMeshOnlySocketList();

		for (int32 j = 0, BoneMax = PoseOverride.Overrides.Num(); j < BoneMax; ++j)
		{
			const FName& BoneName = PoseOverride.Overrides[j].BoneName;
			int32 SourceBoneIndex = SourceSkeleton.FindBoneIndex(BoneName);

			if (SourceBoneIndex != INDEX_NONE)
			{
				bool bOverrideBone = (PoseOverride.Overrides[j].OverrideMode == FRefPoseOverride::ChildrenOnly) ? false : true;

				if (bOverrideBone)
				{
					OverrideBoneSockets(BoneName, SkeletonSocketList);
					OverrideBoneSockets(BoneName, MeshSocketList);
				}

				bool bOverrideChildren = (PoseOverride.Overrides[j].OverrideMode == FRefPoseOverride::BoneOnly) ? false : true;

				if (bOverrideChildren)
				{
					for (int32 ChildBoneIndex = SourceBoneIndex + 1; ChildBoneIndex < SourceSkeleton.GetRawBoneNum(); ++ChildBoneIndex)
					{
						if (SourceSkeleton.BoneIsChildOf(ChildBoneIndex, SourceBoneIndex))
						{
							FName ChildBoneName = SourceSkeleton.GetBoneName(ChildBoneIndex);

							OverrideBoneSockets(ChildBoneName, SkeletonSocketList);
							OverrideBoneSockets(ChildBoneName, MeshSocketList);
						}
					}
				}
			}
		}
	}
}
#pragma optimize("", on)
