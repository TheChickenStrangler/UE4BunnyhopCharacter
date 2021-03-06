// Copyright© SD5 - Sean Dewar, 2015.

#include "SD5BunnyGun.h"
#include "SD5BunnyGunCharacter.h"
#include "SD5BunnyGunCharacterMovement.h"
#include "SD5BunnyGunDmgType_FallDamage.h"
#include "UnrealNetwork.h"

FHitInfo::FHitInfo() :
ActualDamage(0.0f),
bIsKillingHit(false),
DamageTypeClass(nullptr),
InstigatorPawn(nullptr),
DamageCauser(nullptr)
{ }

// Log category for the SD5BunnyGunCharacter
DEFINE_LOG_CATEGORY_STATIC(LogSD5BunnyGunCharacter, Log, All);

void ASD5BunnyGunCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// We will only replicate this to clients that don't own the character as they will not be able to
	// predict the look because they dont get the Controller!
	DOREPLIFETIME_CONDITION(ASD5BunnyGunCharacter, LookRotation, COND_SkipOwner);

	DOREPLIFETIME(ASD5BunnyGunCharacter, Health);
	DOREPLIFETIME_CONDITION(ASD5BunnyGunCharacter, LastHitInfo, COND_Custom);

	DOREPLIFETIME(ASD5BunnyGunCharacter, bUseAutoHop);

	DOREPLIFETIME(ASD5BunnyGunCharacter, bEnableFallDamage);
	DOREPLIFETIME(ASD5BunnyGunCharacter, FallDamageMinZVelocity);
	DOREPLIFETIME(ASD5BunnyGunCharacter, FallDamageZVelocityMultiplier);
}

void ASD5BunnyGunCharacter::PreReplication(IRepChangedPropertyTracker& ChangedPropertyTracker)
{
	Super::PreReplication(ChangedPropertyTracker);

	if (GetWorld() == nullptr)
	{
		return;
	}

	// This is done so that hit fx aren't spammed to clients joining mid-game.
	DOREPLIFETIME_ACTIVE_OVERRIDE(ASD5BunnyGunCharacter, LastHitInfo, (GetWorld()->GetTimeSeconds() < LastHitInfoTimeoutStamp));
}

// Sets default values
ASD5BunnyGunCharacter::ASD5BunnyGunCharacter(const FObjectInitializer& ObjectInitializer) :
Super(ObjectInitializer.SetDefaultSubobjectClass<USD5BunnyGunCharacterMovement>(ASD5BunnyGunCharacter::CharacterMovementComponentName)),
BaseLookUpRate(45.0f),
BaseTurnRate(45.0f),
Health(1000.0f),
bIsDying(false),
bUseAutoHop(true),
bEnableFallDamage(true),
bUseAccelerationForFallDamageCameraTilt(true),
FallDamageMinZVelocity(700.0f),
FallDamageZVelocityMultiplier(0.6f),
FallDamageCameraTiltMultiplier(0.125f),
MaxFallDamageCameraTilt(20.0f),
FallDamageCameraTiltDecayMultiplier(25.0f),
LastHitInfoTimeoutStamp(0.0f)
{
 	// Set this character to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

	// Ensure replication
	bReplicates = true;

	// Init capsule size.
	GetCapsuleComponent()->InitCapsuleSize(30.0f, 72.0f);

	CrouchedEyeHeight = 32.0f;
	BaseEyeHeight = 62.0f;

	// Create fps camera
	FirstPersonCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
	FirstPersonCamera->AttachTo(GetCapsuleComponent());
	// FirstPersonCamera->bUsePawnControlRotation = true;

	// We don't want to see our world model in first person
	GetMesh()->bOwnerNoSee = true;
	GetMesh()->bOnlyOwnerSee = false;

	// Show the shadow of the world model even if we have it hidden - i think it looks kinda cool.
	GetMesh()->bCastHiddenShadow = true;

	// Create text that can be position above player head .etc
	PlayerText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("PlayerText"));
	PlayerText->AttachTo(GetCapsuleComponent());

	PlayerText->SetText(FText::FromString(TEXT("PlayerText")));
	PlayerText->HorizontalAlignment = EHorizTextAligment::EHTA_Center;

	// Hide the text for the owning player.
	PlayerText->bOwnerNoSee = true;
	PlayerText->bOnlyOwnerSee = false;

	// Don't allow player text to receive decals (such as blood and whatever)
	PlayerText->bReceivesDecals = false;
}

void ASD5BunnyGunCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (FirstPersonCamera->bUsePawnControlRotation)
	{
		UE_LOG(LogSD5BunnyGunCharacter, Warning, TEXT("bUsePawnControlRotation set to true for ASD5BunnyGunCharacter! This means certain camera anims will not be able to play (such as fall damage camera tilting .etc)!"));
	}

	PlayerText->SetText(FText::GetEmpty());
}

void ASD5BunnyGunCharacter::Tick( float DeltaTime )
{
	UpdateCamera();
	UpdateLookRotation();

	Super::Tick( DeltaTime );

	UpdateFallDamageCameraTilt(DeltaTime);

	// TODO HACK HACK !!! This shouldn't be called every tick!
	if (PlayerState != nullptr)
	{
		PlayerText->SetText(FText::FromString(FString(PlayerState->PlayerName).Append(", HP: ").Append(FString::SanitizeFloat(Health))));
	}
}

void ASD5BunnyGunCharacter::SetupPlayerInputComponent(class UInputComponent* InputComponent)
{
	Super::SetupPlayerInputComponent(InputComponent);

	check(InputComponent);

	// Bind jump
	InputComponent->BindAction(TEXT("Jump"), IE_Pressed, this, &ACharacter::Jump);
	InputComponent->BindAction(TEXT("Jump"), IE_Released, this, &ACharacter::StopJumping);

	// Bind crouch
	InputComponent->BindAction(TEXT("Crouch"), IE_Pressed, this, &ASD5BunnyGunCharacter::StartCrouching);
	InputComponent->BindAction(TEXT("Crouch"), IE_Released, this, &ASD5BunnyGunCharacter::StopCrouching);
	
	// Bind slow walking
	InputComponent->BindAction(TEXT("SlowWalk"), IE_Pressed, this, &ASD5BunnyGunCharacter::StartSlowWalking);
	InputComponent->BindAction(TEXT("SlowWalk"), IE_Released, this, &ASD5BunnyGunCharacter::StopSlowWalking);

	// Bind suicide
	InputComponent->BindAction(TEXT("Suicide"), IE_Pressed, this, &ASD5BunnyGunCharacter::Suicide);

	// Bind movement
	InputComponent->BindAxis(TEXT("MoveForward"), this, &ASD5BunnyGunCharacter::MoveForward);
	InputComponent->BindAxis(TEXT("MoveRight"), this, &ASD5BunnyGunCharacter::MoveRight);

	// Bind turning for mice
	InputComponent->BindAxis(TEXT("Turn"), this, &APawn::AddControllerYawInput);
	InputComponent->BindAxis(TEXT("LookUp"), this, &APawn::AddControllerPitchInput);

	// Bind turning for joysticks .etc
	InputComponent->BindAxis(TEXT("TurnAtRate"), this, &ASD5BunnyGunCharacter::TurnAtRate);
	InputComponent->BindAxis(TEXT("LookUpAtRate"), this, &ASD5BunnyGunCharacter::LookUpAtRate);
}

void ASD5BunnyGunCharacter::UpdateCamera()
{
	// We need a valid player controller in order to get the camera's rotation.
	const auto ViewRotation = GetViewRotation();
	if (!ViewRotation.Equals(FirstPersonCamera->GetComponentRotation()))
	{
		FirstPersonCamera->SetWorldRotation(ViewRotation);
	}
}

void ASD5BunnyGunCharacter::UpdateLookRotation()
{
	// Only update the look rotation for ourselves if we have a valid controller.
	//
	// If we are not the owning player or server, we won't have a valid player controller to update the value with.
	// Rely on the server to replicate the look direction to us instead.
	if (GetController() != nullptr)
	{
		LookRotation = GetControlRotation();
	}
}

float ASD5BunnyGunCharacter::TakeDamage(float Damage, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	// If we have no health, then deal no damage and do not consider the event.
	// Do not consider taking damage either if we shouldn't be taking damage (if we are not the authority .etc - ShouldTakeDamage() handles this).
	if (Health <= 0.0f || !ShouldTakeDamage(Damage, DamageEvent, EventInstigator, DamageCauser))
	{
		return 0.0f;
	}

	// Get the actual amount of damage we should take.
	const auto ActualDamage = Super::TakeDamage(Damage, DamageEvent, EventInstigator, DamageCauser);
	if (ActualDamage > 0.0f)
	{
		Health -= ActualDamage;

		const auto bIsKillingHit = !IsAlive();

		// Get the damage type. If DamageEvent.DamageTypeClass is null, a default version of UDamageType will be used.
		const auto DamageTypeClass = (DamageEvent.DamageTypeClass != nullptr ? DamageEvent.DamageTypeClass : UDamageType::StaticClass());

		// NOTE: We pass the instigator's pawn instead as Controllers aren't replicated to all clients.
		HandleHit(ActualDamage, bIsKillingHit, DamageTypeClass, (EventInstigator != nullptr ? EventInstigator->GetPawn() : nullptr), DamageCauser);
		if (bIsKillingHit)
		{
			HandleDeath(ActualDamage, DamageTypeClass, EventInstigator, DamageCauser);
		}

		// Alert AI that the person who attacked us made noise. (If null EventInstigator, we make the noise instead).
		MakeNoise(1.0f, (EventInstigator != nullptr ? EventInstigator->GetPawn() : this));
	}

	return ActualDamage;
}

void ASD5BunnyGunCharacter::OnRep_LastHitInfo()
{
	// Handle hit.
	OnHit(LastHitInfo.ActualDamage, LastHitInfo.bIsKillingHit, LastHitInfo.DamageTypeClass, LastHitInfo.InstigatorPawn.Get(), LastHitInfo.DamageCauser.Get());
	if (LastHitInfo.bIsKillingHit)
	{
		// Player was killed by this hit - kill the player.
		OnDeath(LastHitInfo.ActualDamage, LastHitInfo.DamageTypeClass, LastHitInfo.InstigatorPawn.Get(), LastHitInfo.DamageCauser.Get());
	}
}

void ASD5BunnyGunCharacter::HandleHit(float ActualDamage, bool bIsKillingHit, const TSubclassOf<UDamageType>& DamageTypeClass, APawn* InstigatorPawn, AActor* DamageCauser)
{
	const auto TimeoutTimestamp = GetWorld()->GetTimeSeconds() + LAST_HIT_INFO_TIMEOUT_SECONDS;

	// Check if we have another hit by the same instigator in the same frame of the same damage type.
	// If we do, combine the amount of damage we do.
	if (InstigatorPawn == LastHitInfo.InstigatorPawn.Get() &&
		DamageTypeClass == LastHitInfo.DamageTypeClass &&
		LastHitInfoTimeoutStamp == TimeoutTimestamp)
	{
		LastHitInfo.ActualDamage += ActualDamage;
	}
	else
	{
		LastHitInfo.ActualDamage = ActualDamage;
	}

	LastHitInfo.bIsKillingHit = bIsKillingHit;
	LastHitInfo.DamageTypeClass = DamageTypeClass;
	LastHitInfo.InstigatorPawn = InstigatorPawn;
	LastHitInfo.DamageCauser = DamageCauser;

	LastHitInfoTimeoutStamp = TimeoutTimestamp;

	OnHit(ActualDamage, bIsKillingHit, DamageTypeClass, InstigatorPawn, DamageCauser);
}

void ASD5BunnyGunCharacter::OnHit(float Damage, bool bIsKillingHit, const TSubclassOf<UDamageType>& DamageTypeClass, APawn* InstigatorPawn, AActor* DamageCauser)
{
	if (!bIsKillingHit)
	{
		// Play hit sound
		// TODO CHECK IF NOT FALLING DAMAGE!!
		if (GetNetMode() != NM_DedicatedServer && HitSound != nullptr)
		{
			UGameplayStatics::PlaySoundAttached(HitSound, GetRootComponent());
		}
	}
}

bool ASD5BunnyGunCharacter::HandleDeath(float ActualDamage, const TSubclassOf<UDamageType>& DamageTypeClass, AController* Killer, AActor* DamageCauser)
{
	// Check if we can currently die right now...
	if (Role != ROLE_Authority ||					// We MUST be the authority
		bIsDying ||									// We MUST NOT already be in the process of dying
		IsPendingKill() ||							// Our actor must NOT already be in the process of being deleted
		GetWorld()->GetAuthGameMode() == nullptr)	// We MUST have a valid game mode
	{
		return false;
	}

	// The map MUST NOT be changing.
	if (GetWorld()->GetAuthGameMode()->GetMatchState() == MatchState::LeavingMap)
	{
		return false;
	}

	// Make sure health is 0 or less.
	Health = FMath::Min(Health, 0.0f);

	// Check if this is an environmental death. If it is, we refer to the last damage instigator and give kill credit to them.
	Killer = GetDamageInstigator(Killer, *DamageTypeClass.GetDefaultObject());

	// Get the controller of the killed player. If we have no valid controller, get the owner and try to cast it to a controller.
	const auto KilledPlayer = (GetController() != nullptr ? GetController() : Cast<AController>(GetOwner()));
	// TODO Notify gamemode of a kill

	// Reset network update freq and force a movement replication update.
	NetUpdateFrequency = GetDefault<ASD5BunnyGunCharacter>()->NetUpdateFrequency;
	GetCharacterMovement()->ForceReplicationUpdate();

	// NOTE: We pass the instigator's pawn instead as Controllers aren't replicated to all clients.
	OnDeath(ActualDamage, DamageTypeClass, (Killer != nullptr ? Killer->GetPawn() : nullptr), DamageCauser);

	return true;
}

void ASD5BunnyGunCharacter::Kill(const FDamageEvent& DamageEvent, AController* Killer, AActor* DamageCauser)
{
	TakeDamage(Health, DamageEvent, Killer, DamageCauser);
}

void ASD5BunnyGunCharacter::OnDeath(float KillingDamage, const TSubclassOf<UDamageType>& DamageTypeClass, APawn* KillerPawn, AActor* DamageCauser)
{
	// Check that we are not already in the process of dying...
	if (bIsDying)
	{
		return;
	}

	bIsDying = true;

	bReplicateMovement = false;
	bTearOff = true;

	// Play the death sound.
	if (GetNetMode() != NM_DedicatedServer && DeathSound != nullptr)
	{
		UGameplayStatics::PlaySoundAtLocation(this, DeathSound, GetActorLocation());
	}

	// Detach the character pawn from the player controller safetly.
	DetachFromControllerPendingDestroy();

	// Turn our collision off for our capsule.
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GetCapsuleComponent()->SetCollisionResponseToAllChannels(ECR_Ignore);

	// Set our world mesh's collision to ragdoll mode.
	if (GetMesh() != nullptr)
	{
		GetMesh()->SetCollisionProfileName(TEXT("Ragdoll"));
	}

	// Enable collision for our actor if it was previously disabled.
	SetActorEnableCollision(true);
	
	// Ragdoll the character.
	if (RagdollCharacter())
	{
		// Let the ragdoll stay around for a decent amount of time.
		SetLifeSpan(15.0f);
	}
	else
	{
		// If we were not able to ragdoll, turn off the character and stay around for a short time.
		TurnOff();
		SetActorHiddenInGame(true);
		SetLifeSpan(0.1f);
	}
}

bool ASD5BunnyGunCharacter::IsAlive() const
{
	return (Health > 0.0f);
}

bool ASD5BunnyGunCharacter::RagdollCharacter()
{
	auto bCanRagdoll = true;

	// Do not ragdoll if we have no mesh or are about to be deleted.
	if (IsPendingKill() || GetMesh() == nullptr)
	{
		bCanRagdoll = false;
	}

	// Do not ragdoll if we have no physics asset for our mesh (or we won't be able to ragdoll).
	if (GetMesh()->GetPhysicsAsset() == nullptr)
	{
		bCanRagdoll = false;
	}

	// Stop movement and movement logic.
	GetCharacterMovement()->StopMovementImmediately();
	GetCharacterMovement()->DisableMovement();
	GetCharacterMovement()->SetComponentTickEnabled(false);

	if (bCanRagdoll)
	{
		// Init the ragdoll and make sure all bodies are ragdolled / simulating physics too.
		GetMesh()->SetAllBodiesSimulatePhysics(true);
		GetMesh()->SetSimulatePhysics(true);
		GetMesh()->WakeAllRigidBodies();
		GetMesh()->bBlendPhysics = true;
	}

	// Return whether or not the character successfully ragdolled.
	return bCanRagdoll;
}

void ASD5BunnyGunCharacter::Suicide()
{
	if (Role >= ROLE_AutonomousProxy)
	{
		ServerSuicide();
	}
}

void ASD5BunnyGunCharacter::ServerSuicide_Implementation()
{
	if (!bIsDying)
	{
		Kill(FDamageEvent(), GetController(), this);
	}
}

bool ASD5BunnyGunCharacter::ServerSuicide_Validate()
{
	return true;
}

void ASD5BunnyGunCharacter::OnJumped_Implementation()
{
	Super::OnJumped_Implementation();

	PlayJumpSound();
}

void ASD5BunnyGunCharacter::UpdateFallDamageCameraTilt(float DeltaTime)
{
	if (GetNetMode() == NM_DedicatedServer || GetController() == nullptr)
	{
		return;
	}

	// Nothing to do if there is no previous tilt.
	if (FallDamageCameraTilt == 0.0f)
	{
		return;
	}

	if (FMath::IsNegativeFloat(FallDamageCameraTilt))
	{
		FallDamageCameraTilt += DeltaTime * FallDamageCameraTiltDecayMultiplier;
		FallDamageCameraTilt = FMath::Min(FallDamageCameraTilt, 0.0f);
	}
	else
	{
		FallDamageCameraTilt -= DeltaTime * FallDamageCameraTiltDecayMultiplier;
		FallDamageCameraTilt = FMath::Max(0.0f, FallDamageCameraTilt);
	}

	// Update the tilt of the camera.
	const auto AddRotation = FRotator(0.0f, 0.0f, FallDamageCameraTilt);
	FirstPersonCamera->AddRelativeRotation(AddRotation);
}

void ASD5BunnyGunCharacter::HandleFallDamage()
{
	if (!bEnableFallDamage || Role < ROLE_AutonomousProxy)
	{
		return;
	}

	// We need to calculate the fall damage on the client too so that we can do visual effects.
	// The client will not apply the damage, though (the server/authority will).
	const auto Velocity = GetVelocity();
	const auto FallDamage = (-Velocity.Z - FallDamageMinZVelocity) * FallDamageZVelocityMultiplier;
	if (FallDamage <= 0.0f)
	{
		return;
	}

	// Apply visual camera tilt & sound effects .etc
	if (GetNetMode() != NM_DedicatedServer)
	{
		// Get the cosine of the angle between the right vector & horiz velo/accel direction.
		// This will be used to determine which way to tilt the camera (to the left or to the right).
		auto HorizDirection = FVector::ZeroVector;
		if (bUseAccelerationForFallDamageCameraTilt)
		{
			auto Acceleration = FVector::ZeroVector;
			if (GetCharacterMovement() != nullptr)
			{
				Acceleration = GetCharacterMovement()->GetCurrentAcceleration();
			}

			HorizDirection = FVector(Acceleration.X, Acceleration.Y, 0.0f).GetSafeNormal();
		}
		else
		{
			HorizDirection = FVector(Velocity.X, Velocity.Y, 0.0f).GetSafeNormal();
		}

		const auto AngleCosine = FVector::DotProduct(GetActorRightVector(), HorizDirection);

		// Assume a tilt to the right (+ve FallDamageCameraTilt)
		FallDamageCameraTilt = FallDamage * FallDamageCameraTiltMultiplier;
		if (AngleCosine < -SMALL_NUMBER)
		{
			// Tilt to the left (-ve FallDamageCameraTilt)
			FallDamageCameraTilt = -FallDamageCameraTilt;
		}
		else if (AngleCosine >= -SMALL_NUMBER && AngleCosine <= SMALL_NUMBER)
		{
			// Tilt in a random direction (+ve or -ve)
			if (FMath::RandBool())
			{
				FallDamageCameraTilt = -FallDamageCameraTilt;
			}
		}

		// Cap tilt value to max value.
		if (FMath::IsNegativeFloat(FallDamageCameraTilt))
		{
			// Compare against the negative version of the max cap.
			FallDamageCameraTilt = FMath::Max(FallDamageCameraTilt, -MaxFallDamageCameraTilt);
		}
		else
		{
			// Compare against the positive version of the max cap.
			FallDamageCameraTilt = FMath::Min(FallDamageCameraTilt, MaxFallDamageCameraTilt);
		}
	}

	// We should be taking fall damage at this point, so play the fall damage sound.
	// TODO Handle this somewhere else aka OnHit and use damage type to play sound ?!?!?!
	if (GetNetMode() != NM_DedicatedServer && FallDamageSound != nullptr)
	{
		UGameplayStatics::PlaySoundAttached(FallDamageSound, GetRootComponent());
	}

	// Apply the actual fall damage.
	if (Role == ROLE_Authority)
	{
		TakeDamage(FallDamage, FDamageEvent(USD5BunnyGunDmgType_FallDamage::StaticClass()));
	}
}

void ASD5BunnyGunCharacter::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);

	// Handles fall damage if necessary.
	HandleFallDamage();
}

void ASD5BunnyGunCharacter::PlayJumpSound()
{
	if (GetNetMode() != NM_DedicatedServer && JumpSound != nullptr)
	{
		if (JumpAudio != nullptr)
		{
			JumpAudio->Stop();
		}

		if (JumpAudio == nullptr)
		{
			JumpAudio = UGameplayStatics::PlaySoundAttached(JumpSound, GetRootComponent());
			if (JumpAudio != nullptr)
			{
				JumpAudio->bAutoDestroy = false;
			}
		}
		else
		{
			JumpAudio->Play();
		}
	}
}

void ASD5BunnyGunCharacter::ClearJumpInput()
{
	// Do not clear jump input if autohop is on.
	if (bPressedJump && bUseAutoHop)
	{
		JumpKeyHoldTime = 0.0f;
		return;
	}

	Super::ClearJumpInput();
}

void ASD5BunnyGunCharacter::StartCrouching()
{
	if (CanCrouch())
	{
		Crouch();
	}
}

void ASD5BunnyGunCharacter::StopCrouching()
{
	UnCrouch();
}

void ASD5BunnyGunCharacter::StartSlowWalking()
{
	if (GetMovementComponent() == nullptr)
	{
		return;
	}

	const auto MoveComponent = static_cast<USD5BunnyGunCharacterMovement*>(GetMovementComponent());
	if (MoveComponent->bCanSlowWalk)
	{
		MoveComponent->SetSlowWalking(true);
	}
}

void ASD5BunnyGunCharacter::StopSlowWalking()
{
	if (GetMovementComponent() == nullptr)
	{
		return;
	}

	const auto MoveComponent = static_cast<USD5BunnyGunCharacterMovement*>(GetMovementComponent());
	if (MoveComponent->bCanSlowWalk)
	{
		MoveComponent->SetSlowWalking(false);
	}
}

void ASD5BunnyGunCharacter::TurnAtRate(float Rate)
{
	if (Rate != 0.0f)
	{
		AddControllerYawInput(Rate * BaseTurnRate * GetWorld()->GetDeltaSeconds());
	}
}

void ASD5BunnyGunCharacter::LookUpAtRate(float Rate)
{
	if (Rate != 0.0f)
	{
		AddControllerPitchInput(Rate * BaseLookUpRate * GetWorld()->GetDeltaSeconds());
	}
}

void ASD5BunnyGunCharacter::MoveForward(float Val)
{
	if (Val != 0.0f)
	{
		AddMovementInput(GetActorForwardVector(), Val);
	}
}

void ASD5BunnyGunCharacter::MoveRight(float Val)
{
	if (Val != 0.0f)
	{
		AddMovementInput(GetActorRightVector(), Val);
	}
}