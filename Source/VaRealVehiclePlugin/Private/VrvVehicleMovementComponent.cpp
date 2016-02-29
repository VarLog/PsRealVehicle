// Copyright 2016 Vladimir Alyamkin. All Rights Reserved.

#include "VrvPlugin.h"

#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"

UVrvVehicleMovementComponent::UVrvVehicleMovementComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bAutoActivate = true;
	bWantsInitializeComponent = true;
	PrimaryComponentTick.bCanEverTick = true;

	SprocketMass = 65.f;
	SprocketRadius = 25.f;
	TrackMass = 600.f;

	bAutoGear = true;

	BrakeForce = 30.f;

	DifferentialRatio = 3.5f;
	TransmissionEfficiency = 0.9f;
	EngineExtraPowerRatio = 3.f;

	// Init basic torque curve
	FRichCurve* TorqueCurveData = EngineTorqueCurve.GetRichCurve();
	TorqueCurveData->AddKey(0.f, 800.f);
	TorqueCurveData->AddKey(1400.f, 850.f);
	TorqueCurveData->AddKey(2800.f, 800.f);
	TorqueCurveData->AddKey(2810.f, 0.f);
}

//////////////////////////////////////////////////////////////////////////
// Initialization

void UVrvVehicleMovementComponent::InitializeComponent()
{
	Super::InitializeComponent();

	CalculateMOI();
	InitSuspension();
	InitGears();
}

void UVrvVehicleMovementComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	UpdateThrottle(DeltaTime);
	UpdateTracksVelocity(DeltaTime);
	UpdateHullVelocity(DeltaTime);
	UpdateEngine(DeltaTime);
	UpdateDriveForce(DeltaTime);
	UpdateSuspension(DeltaTime);

	// @todo Reset input

	// Show debug
	if (bShowDebug)
	{
		DrawDebugLines();
	}
}


//////////////////////////////////////////////////////////////////////////
// Physics Initialization

void UVrvVehicleMovementComponent::CalculateMOI()
{
	float SprocketSquareRadius = (SprocketRadius * SprocketRadius);
	float SprocketMOI = (SprocketMass / 2) * SprocketSquareRadius;
	float TrackMOI = TrackMass * SprocketSquareRadius;

	FinalMOI = SprocketMOI + TrackMOI;

	UE_LOG(LogVrvVehicle, Warning, TEXT("Final MOI: %f"), FinalMOI);
}

void UVrvVehicleMovementComponent::InitSuspension()
{
	for (auto SuspInfo : SuspensionSetup)
	{
		if (SuspInfo.bInheritWheelBoneTransform)
		{
			USkinnedMeshComponent* Mesh = GetMesh();
			if (Mesh)
			{
				FTransform WheelTransform = Mesh->GetSocketTransform(SuspInfo.BoneName, RTS_Actor);
				SuspInfo.Location = WheelTransform.GetLocation();
				SuspInfo.Rotation = WheelTransform.GetRotation().Rotator();

				UE_LOG(LogVrvVehicle, Log, TEXT("Init suspension (%s): %s"), *SuspInfo.BoneName.ToString(), *SuspInfo.Location.ToString());
			}
		}

		FSuspensionState SuspState;
		SuspState.SuspensionInfo = SuspInfo;
		SuspState.PreviousLength = SuspInfo.Length;

		SuspensionData.Add(SuspState);
	}
}

void UVrvVehicleMovementComponent::InitGears()
{
	for (int32 i = 0; i < GearSetup.Num(); i++)
	{
		if (GearSetup[i].Ratio == 0.f)
		{
			NeutralGear = (bAutoGear) ? i : FMath::Max(i + 1, GearSetup.Num());
			break;
		}
	}

	UE_LOG(LogVrvVehicle, Warning, TEXT("Neutral gear: %d"), NeutralGear);
}


//////////////////////////////////////////////////////////////////////////
// Physics simulation

void UVrvVehicleMovementComponent::UpdateThrottle(float DeltaTime)
{
	// @todo Throttle shouldn't be instant
	ThrottleInput = RawThrottleInput;

	// Calc torque transfer based on input
	LeftTrack.TorqueTransfer = FMath::Abs(ThrottleInput) + LeftTrack.Input;
	RightTrack.TorqueTransfer = FMath::Abs(ThrottleInput) + RightTrack.Input;
}

void UVrvVehicleMovementComponent::UpdateTracksVelocity(float DeltaTime)
{
	// Calc total torque
	RightTrackTorque = RightTrack.DriveTorque + RightTrack.FrictionTorque + RightTrack.RollingFrictionTorque;
	LeftTrackTorque = LeftTrack.DriveTorque + LeftTrack.FrictionTorque + LeftTrack.RollingFrictionTorque;

	// Update right track velocity
	float AngularVelocity = RightTrack.AngularVelocity + RightTrackTorque / FinalMOI * DeltaTime;
	RightTrack.AngularVelocity = ApplyBrake(DeltaTime, AngularVelocity, RightTrack.BrakeRatio);
	RightTrack.LinearVelocity = RightTrack.AngularVelocity * SprocketRadius;

	// Update left track velocity
	AngularVelocity = LeftTrack.AngularVelocity + LeftTrackTorque / FinalMOI * DeltaTime;
	LeftTrack.AngularVelocity = ApplyBrake(DeltaTime, AngularVelocity, LeftTrack.BrakeRatio);
	LeftTrack.LinearVelocity = LeftTrack.AngularVelocity * SprocketRadius;
}

void UVrvVehicleMovementComponent::UpdateHullVelocity(float DeltaTime)
{
	HullAngularVelocity = (FMath::Abs(LeftTrack.AngularVelocity) + FMath::Abs(RightTrack.AngularVelocity)) / 2.f;
}

void UVrvVehicleMovementComponent::UpdateEngine(float DeltaTime)
{
	FGearInfo CurrentGear = GetGearInfo(GetCurrentGear());

	// @todo Cache engine RPM limits
	float MinRPM, MaxRPM;
	FRichCurve* TorqueCurveData = EngineTorqueCurve.GetRichCurve();
	TorqueCurveData->GetTimeRange(MinRPM, MaxRPM);

	// Update engine rotation speed (RPM)
	EngineRPM = OmegaToRPM((CurrentGear.Ratio * DifferentialRatio) * HullAngularVelocity);
	EngineRPM = FMath::Clamp(EngineRPM, MinRPM, MaxRPM);

	// Calculate engine torque based on current RPM
	float MaxEngineTorque = TorqueCurveData->Eval(EngineRPM);
	MaxEngineTorque *= 100.f; // From Meters to Cm
	EngineTorque = MaxEngineTorque * ThrottleInput;

	// Gear box torque
	DriveTorque = EngineTorque * CurrentGear.Ratio * DifferentialRatio * TransmissionEfficiency;
	DriveTorque *= (bReverseGear) ? -1.f : 1.f;
	DriveTorque *= EngineExtraPowerRatio;
}

void UVrvVehicleMovementComponent::UpdateDriveForce(float DeltaTime)
{
	// Drive force (right)
	RightTrack.DriveTorque = RightTrack.TorqueTransfer * DriveTorque;
	RightTrack.DriveForce = UpdatedComponent->GetForwardVector() * (RightTrack.DriveTorque / SprocketRadius);

	// Drive force (left)
	LeftTrack.DriveTorque = LeftTrack.TorqueTransfer * DriveTorque;
	LeftTrack.DriveForce = UpdatedComponent->GetForwardVector() * (LeftTrack.DriveTorque / SprocketRadius);
}

void UVrvVehicleMovementComponent::UpdateSuspension(float DeltaTime)
{
	// Refresh friction points counter
	ActiveFrictionPoints = 0;

	for (auto& SuspState : SuspensionData)
	{
		const FVector SuspUpVector = UpdatedComponent->GetComponentTransform().TransformVectorNoScale(UKismetMathLibrary::GetUpVector(SuspState.SuspensionInfo.Rotation));
		const FVector SuspWorldLocation = UpdatedComponent->GetComponentTransform().TransformPosition(SuspState.SuspensionInfo.Location);
		const FVector SuspTraceEndLocation = SuspWorldLocation - SuspUpVector * SuspState.SuspensionInfo.Length;

		// Make trace to touch the ground
		FHitResult Hit;
		TArray<AActor*> IgnoredActors;
		EDrawDebugTrace::Type DebugType = IsDebug() ? EDrawDebugTrace::ForOneFrame : EDrawDebugTrace::None;
		bool bHit = UKismetSystemLibrary::SphereTraceSingle_NEW(this, SuspWorldLocation, SuspTraceEndLocation, SuspState.SuspensionInfo.CollisionRadius,
			UEngineTypes::ConvertToTraceType(ECollisionChannel::ECC_Visibility), false, IgnoredActors, DebugType, Hit, true);

		// Process hit results
		if (bHit) 
		{
			float NewSuspensionLength = (SuspWorldLocation - Hit.Location).Size();

			float SpringCompressionRatio = FMath::Clamp((SuspState.SuspensionInfo.Length - NewSuspensionLength) / SuspState.SuspensionInfo.Length, 0.f, 1.f);
			float TargetVelocity = 0.f;		// @todo Target velocity can be different for wheeled vehicles
			float SuspensionVelocity = (NewSuspensionLength - SuspState.PreviousLength) / DeltaTime;
			float SuspensionForce = (TargetVelocity - SuspensionVelocity) * SuspState.SuspensionInfo.Damping + SpringCompressionRatio * SuspState.SuspensionInfo.Stiffness;

			SuspState.SuspensionForce = SuspensionForce * SuspUpVector;
			SuspState.WheelCollisionLocation = Hit.ImpactPoint;
			SuspState.WheelCollisionNormal = Hit.ImpactNormal;
			SuspState.PreviousLength = NewSuspensionLength;
			SuspState.WheelTouchedGround = true;
			SuspState.SurfaceType = UGameplayStatics::GetSurfaceType(Hit);

			ActiveFrictionPoints++;
		}
		else
		{
			// If there is no collision then suspension is relaxed
			SuspState.SuspensionForce = FVector::ZeroVector;
			SuspState.WheelCollisionLocation = FVector::ZeroVector;
			SuspState.WheelCollisionNormal = FVector::UpVector;
			SuspState.PreviousLength = SuspState.SuspensionInfo.Length;
			SuspState.WheelTouchedGround = false;
			SuspState.SurfaceType = EPhysicalSurface::SurfaceType_Default;
		}

		// Add suspension force if spring compressed
		if (!SuspState.SuspensionForce.IsZero())
		{
			GetMesh()->AddForceAtLocation(SuspState.SuspensionForce, SuspWorldLocation);
		}

		// Push suspension force to environment
		if (bHit)
		{
			UPrimitiveComponent* PrimitiveComponent = Hit.Component.Get();
			if (PrimitiveComponent && PrimitiveComponent->IsSimulatingPhysics()) 
			{
				PrimitiveComponent->AddForceAtLocation(-SuspState.SuspensionForce, SuspWorldLocation);
			}
		}

		// Debug
		if (bShowDebug)
		{
			// Suspension force
			DrawDebugLine(GetWorld(), SuspWorldLocation, SuspWorldLocation + SuspState.SuspensionForce * 0.0001f, FColor::Green, false, /*LifeTime*/ 0.f, /*DepthPriority*/ 0,  /*Thickness*/ 4.f);

			// Suspension length
			DrawDebugPoint(GetWorld(), SuspWorldLocation, 5.f, FColor(0.8f, 0.f, 0.9f, 1.f), false, /*LifeTime*/ 0.f);
			DrawDebugLine(GetWorld(), SuspWorldLocation, SuspWorldLocation - SuspUpVector * SuspState.PreviousLength, FColor::Blue, false, 0.f, 0, 4.f);
			DrawDebugLine(GetWorld(), SuspWorldLocation, SuspWorldLocation - SuspUpVector * SuspState.SuspensionInfo.Length, FColor::Red, false, 0.f, 0, 2.f);
		}
	}
}

float UVrvVehicleMovementComponent::ApplyBrake(float DeltaTime, float AngularVelocity, float BrakeRatio)
{
	float BrakeVelocity = BrakeRatio * BrakeForce * DeltaTime;

	if (FMath::Abs(AngularVelocity) > FMath::Abs(BrakeVelocity)) 
	{
		return (AngularVelocity - (BrakeVelocity * FMath::Sign(BrakeVelocity)));
	}

	return 0.f;
}


//////////////////////////////////////////////////////////////////////////
// Vehicle control

void UVrvVehicleMovementComponent::SetThrottleInput(float Throttle)
{
	RawThrottleInput = FMath::Clamp(Throttle, -1.0f, 1.0f);
}

void UVrvVehicleMovementComponent::SetSteeringInput(float Steering)
{
	RawSteeringInput = FMath::Clamp(Steering, -1.0f, 1.0f);

	// Update tracks coefficients
	SteeringInput = RawSteeringInput;
	LeftTrack.Input = -SteeringInput;
	RightTrack.Input = SteeringInput;
}

void UVrvVehicleMovementComponent::SetHandbrakeInput(bool bNewHandbrake)
{
	bRawHandbrakeInput = bNewHandbrake;
}


//////////////////////////////////////////////////////////////////////////
// Vehicle stats

float UVrvVehicleMovementComponent::GetForwardSpeed() const
{
	return 0.0f;
}

float UVrvVehicleMovementComponent::GetEngineRotationSpeed() const
{
	return 0.0f;
}

float UVrvVehicleMovementComponent::GetEngineMaxRotationSpeed() const
{
	return 0.0f;
}


//////////////////////////////////////////////////////////////////////////
// Data access

USkinnedMeshComponent* UVrvVehicleMovementComponent::GetMesh()
{
	return Cast<USkinnedMeshComponent>(UpdatedComponent);
}

int32 UVrvVehicleMovementComponent::GetCurrentGear() const
{
	return CurrentGear;
}

FGearInfo UVrvVehicleMovementComponent::GetGearInfo(int32 GearNum) const
{
	// Check that requested gear is valid
	if (GearNum < 0 || GearNum >= GearSetup.Num())
	{
		UE_LOG(LogVrvVehicle, Error, TEXT("Invalid gear index: %d from %d"), GearNum, GearSetup.Num());
		return FGearInfo();
	}

	return GearSetup[GearNum];
}


//////////////////////////////////////////////////////////////////////////
// Debug

void UVrvVehicleMovementComponent::DrawDebug(UCanvas* Canvas, float& YL, float& YPos)
{
	// @todo 
}

void UVrvVehicleMovementComponent::DrawDebugLines()
{
	// Torque transfer balance
	DrawDebugString(GetWorld(), UpdatedComponent->GetComponentTransform().TransformPosition(FVector(0.f, -100.f, 0.f)), FString::SanitizeFloat(LeftTrack.TorqueTransfer), nullptr, FColor::White, 0.f);
	DrawDebugString(GetWorld(), UpdatedComponent->GetComponentTransform().TransformPosition(FVector(0.f, 100.f, 0.f)), FString::SanitizeFloat(RightTrack.TorqueTransfer), nullptr, FColor::White, 0.f);

	// Tracks torque
	DrawDebugString(GetWorld(), UpdatedComponent->GetComponentTransform().TransformPosition(FVector(0.f, -300.f, 0.f)), FString::SanitizeFloat(LeftTrackTorque), nullptr, FColor::White, 0.f);
	DrawDebugString(GetWorld(), UpdatedComponent->GetComponentTransform().TransformPosition(FVector(0.f, 300.f, 0.f)), FString::SanitizeFloat(RightTrackTorque), nullptr, FColor::White, 0.f);

	// Center of mass
	DrawDebugPoint(GetWorld(), GetMesh()->GetCenterOfMass(), 25.f, FColor::Yellow, false, /*LifeTime*/ 0.f);
}
