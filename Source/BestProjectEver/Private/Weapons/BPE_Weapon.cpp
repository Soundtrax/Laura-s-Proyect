// Fill out your copyright notice in the Description page of Project Settings.


#include "Weapons/BPE_Weapon.h"

#include "Character/BPE_PlayerCharacter.h"
#include "Components/SphereComponent.h"
#include "Components/WidgetComponent.h"
#include "Net/UnrealNetwork.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Kismet/GameplayStatics.h"
#include "Weapons/BPE_Casing.h"
#include "Weapons/BPE_Projectile.h"

ABPE_Weapon::ABPE_Weapon()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	SetReplicatingMovement(true);

	WeaponMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("WeaponMesh"));
	SetRootComponent(WeaponMesh);
	WeaponMesh->SetCollisionResponseToAllChannels(ECR_Block);
	WeaponMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	WeaponMesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	WeaponMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	WeaponMesh->SetSimulatePhysics(false);
	WeaponMesh->SetEnableGravity(false);
	
	PickupArea = CreateDefaultSubobject<USphereComponent>(TEXT("PickupArea"));
	PickupArea->SetupAttachment(RootComponent);
	PickupArea->SetCollisionResponseToAllChannels(ECR_Ignore);
	PickupArea->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	PickupWidget = CreateDefaultSubobject<UWidgetComponent>(TEXT("PickupWidget"));
	PickupWidget->SetupAttachment(RootComponent);

	CurrentState = EWeaponState::Idle;
	RoundsPerMinute = 600.0f;
	ShotDistance = 10000.0f;

	MuzzleFlashSocketName = "SCK_MuzzleFlash";
	AmmoEjectSocketName = "SCK_AmmoEject";
	
	bCanFire = true;
	bIsAutomatic = true;

	ZoomedFOV = 30.0f;
	ZoomInterpSpeed = 20.0f;
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::BeginPlay()
{
	Super::BeginPlay();
	InitializeReferences();
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::InitializeReferences()
{
	/** check if we are on the server to enable PickupArea collision*/
	if(HasAuthority())
	{
		PickupArea->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
		PickupArea->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		PickupArea->OnComponentBeginOverlap.AddDynamic(this, &ABPE_Weapon::OnPlayerOverlap);
		PickupArea->OnComponentEndOverlap.AddDynamic(this, &ABPE_Weapon::OnPlayerEndOverlap);
	}

	SetWidgetVisibility(false);

	TimeBetweenShots = 60 / RoundsPerMinute;
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::OnPlayerOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	ABPE_PlayerCharacter* OverlappedCharacter = Cast<ABPE_PlayerCharacter>(OtherActor);
	if(IsValid(OverlappedCharacter ))
	{
		OverlappedCharacter->SetOverlappingWeapon(this);
	}
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::OnPlayerEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
	ABPE_PlayerCharacter* OverlappedCharacter  = Cast<ABPE_PlayerCharacter>(OtherActor);
	if(IsValid(OverlappedCharacter))
	{
		OverlappedCharacter->SetOverlappingWeapon(nullptr);
	}
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::OnRep_WeaponState()
{
	OnSetWeaponState();
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::OnSetWeaponState()
{
	switch (CurrentState)
	{
	case EWeaponState::Idle:
		{
			const ECollisionEnabled::Type PickupAreaTypeCollision = HasAuthority()?
				ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision;
			
			SetWeaponParametersOnNewState(ECollisionEnabled::QueryAndPhysics, true,
				PickupAreaTypeCollision);
			break;
		}
	case EWeaponState::Equipped:
		{
			SetWeaponParametersOnNewState(ECollisionEnabled::NoCollision, false,
				ECollisionEnabled::NoCollision);
			break;
		}
	default:
		{
			break;
		}
	}
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::SetWeaponParametersOnNewState(ECollisionEnabled::Type MeshTypeCollision, bool bEnableMeshPhysics,
		ECollisionEnabled::Type PickupAreaTypeCollision)
{
	WeaponMesh->SetSimulatePhysics(bEnableMeshPhysics);
	WeaponMesh->SetEnableGravity(bEnableMeshPhysics);
	WeaponMesh->SetCollisionEnabled(MeshTypeCollision);
	
	PickupArea->SetCollisionEnabled(PickupAreaTypeCollision);

	SetWidgetVisibility(false);
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::Fire()
{
	if(IsValid(WeaponMesh) && IsValid(BulletClass))
	{
		const USkeletalMeshSocket* MuzzleSocket = WeaponMesh->GetSocketByName(MuzzleFlashSocketName);
		if(IsValid(MuzzleSocket))
		{
			const FTransform SocketTransform = MuzzleSocket->GetSocketTransform(WeaponMesh);

			TraceUnderCrosshairs();
			
			const FVector BulletDirection = HitTarget - SocketTransform.GetLocation();
			const FRotator BulletRotation = BulletDirection.Rotation();
			
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.Owner = GetOwner();
			SpawnParameters.Instigator = Cast<APawn>(GetOwner());
			
			GetWorld()->SpawnActor<ABPE_Projectile>(BulletClass, SocketTransform.GetLocation(), BulletRotation, SpawnParameters);
			Multicast_PlayFireEffects(HitTarget);
		}
	}
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::TraceUnderCrosshairs()
{
	TObjectPtr<ABPE_PlayerCharacter> PlayerOwner = Cast<ABPE_PlayerCharacter>(OwnerCharacter);

	if(IsValid(PlayerOwner))
	{
		FTransform CameraLocation = PlayerOwner->GetCameraTransform();

		FVector ShotDirection = CameraLocation.Rotator().Vector();
		
		FVector TraceStart = CameraLocation.GetLocation();
		const float DistanceToPlayer = (PlayerOwner->GetActorLocation() - TraceStart).Size();
		const float ExtraDistance = 100.0f;
		
		/** This additional distance is to prevent the shoot hit something behind the character */
		TraceStart += ShotDirection * (DistanceToPlayer + ExtraDistance);
		
		FVector TraceEnd = CameraLocation.GetLocation() + (ShotDirection * ShotDistance);

		FCollisionQueryParams QueryParams;
		QueryParams.AddIgnoredActor(Owner);
		QueryParams.AddIgnoredActor(this);
		QueryParams.bTraceComplex = true;

		FHitResult HitResult;
		GetWorld()->LineTraceSingleByChannel(HitResult, CameraLocation.GetLocation(), TraceEnd, ECC_Visibility);
		DrawDebugLine(GetWorld(), TraceStart, TraceEnd, FColor::White, true, 10.0f, 1.0f, 10.0f);
		HitTarget = HitResult.ImpactPoint;	
	}
}


//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::StopFire()
{
	GetWorldTimerManager().ClearTimer(TimerHandle_AutoFire);
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::HandleReFiring()
{
	bCanFire = true;
	if(bIsAutomatic && CurrentState == EWeaponState::Firing)
	{
		Fire();
	}
	else
	{
		StopFire();
	}
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::SetOwner(AActor* NewOwner)
{
	Super::SetOwner(NewOwner);
	OwnerCharacter = Cast<ABPE_BaseCharacter>(NewOwner);
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::Multicast_PlayFireEffects_Implementation(const FVector ImpactPoint)
{
	if (IsValid(FireAnimation))
	{
		WeaponMesh->PlayAnimation(FireAnimation, false);
	}

	/* to debug */
	const USkeletalMeshSocket* MuzzleSocket = WeaponMesh->GetSocketByName(MuzzleFlashSocketName);
	const FTransform SocketTransform = MuzzleSocket->GetSocketTransform(WeaponMesh);
	DrawDebugLine(GetWorld(), SocketTransform.GetLocation(), ImpactPoint, FColor::White, true, 3.0f, 1.0f, 4.0f);
	/* ... */
	
	if (IsValid(CasingClass))
	{
		const USkeletalMeshSocket* AmmoEjectSocket = WeaponMesh->GetSocketByName(AmmoEjectSocketName);
		if (IsValid(AmmoEjectSocket))
		{
			const FTransform AmmoSocketTransform = AmmoEjectSocket->GetSocketTransform(WeaponMesh);

			GetWorld()->SpawnActor<ABPE_Casing>(CasingClass, AmmoSocketTransform.GetLocation(), AmmoSocketTransform.Rotator());
		}
	}
}

//----------------------------------------------------------------------------------------------------------------------
bool ABPE_Weapon::Multicast_PlayFireEffects_Validate(const FVector ImpactPoint)
{
	return true;
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::StartFire()
{
	if(bCanFire)
	{
		bCanFire = false;
		Fire();
		GetWorldTimerManager().SetTimer(TimerHandle_AutoFire, this, &ABPE_Weapon::HandleReFiring,
			TimeBetweenShots, true, TimeBetweenShots);		
	}
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::SetState(EWeaponState State)
{
	CurrentState = State;
	OnSetWeaponState();
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::SetWidgetVisibility(bool bShowWidget)
{
	if(IsValid(PickupWidget))
	{
		PickupWidget->SetVisibility(bShowWidget);
	}
}

//----------------------------------------------------------------------------------------------------------------------
void ABPE_Weapon::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ABPE_Weapon, CurrentState);
}




