// Copyright 2019 Dan Kestranek.


#include "GDAttributeSetBase.h"
#include "GameplayEffect.h"
#include "GameplayEffectExtension.h"
#include "GDCharacterBase.h"
#include "GDPlayerController.h"
#include "UnrealNetwork.h"

UGDAttributeSetBase::UGDAttributeSetBase()
{
	// Cache tags
	HitDirectionFrontTag = FGameplayTag::RequestGameplayTag(FName("Effect.HitReact.Front"));
	HitDirectionBackTag = FGameplayTag::RequestGameplayTag(FName("Effect.HitReact.Back"));
	HitDirectionRightTag = FGameplayTag::RequestGameplayTag(FName("Effect.HitReact.Right"));
	HitDirectionLeftTag = FGameplayTag::RequestGameplayTag(FName("Effect.HitReact.Left"));
}

void UGDAttributeSetBase::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	// This is called whenever attributes change, so for max health/mana we want to scale the current totals to match
	Super::PreAttributeChange(Attribute, NewValue);

	// If a Max value changes, adjust current to keep Current % of Current to Max
	if (Attribute == GetMaxHealthAttribute()) // GetMaxHealthAttribute comes from the Macros defined at the top of the header
	{
		AdjustAttributeForMaxChange(Health, MaxHealth, NewValue, GetHealthAttribute());
	}
	else if (Attribute == GetMaxManaAttribute())
	{
		AdjustAttributeForMaxChange(Mana, MaxMana, NewValue, GetManaAttribute());
	}
	else if (Attribute == GetMaxStaminaAttribute())
	{
		AdjustAttributeForMaxChange(Stamina, MaxStamina, NewValue, GetStaminaAttribute());
	}
	else if (Attribute == GetMoveSpeedAttribute())
	{
		// Cannot slow less than 150 units/s and cannot boost more than 1000 units/s
		NewValue = FMath::Clamp<float>(NewValue, 150, 1000);
	}
}

void UGDAttributeSetBase::PostGameplayEffectExecute(const FGameplayEffectModCallbackData & Data)
{
	Super::PostGameplayEffectExecute(Data);

	FGameplayEffectContextHandle Context = Data.EffectSpec.GetContext();
	UAbilitySystemComponent* Source = Context.GetOriginalInstigatorAbilitySystemComponent();
	const FGameplayTagContainer& SourceTags = *Data.EffectSpec.CapturedSourceTags.GetAggregatedTags();
	FGameplayTagContainer SpecAssetTags;
	Data.EffectSpec.GetAllAssetTags(SpecAssetTags);

	// Get the Target actor, which should be our owner
	AActor* TargetActor = nullptr;
	AController* TargetController = nullptr;
	AGDCharacterBase* TargetCharacter = nullptr;
	if (Data.Target.AbilityActorInfo.IsValid() && Data.Target.AbilityActorInfo->AvatarActor.IsValid())
	{
		TargetActor = Data.Target.AbilityActorInfo->AvatarActor.Get();
		TargetController = Data.Target.AbilityActorInfo->PlayerController.Get();
		TargetCharacter = Cast<AGDCharacterBase>(TargetActor);
	}

	// Get the Source actor
	AActor* SourceActor = nullptr;
	AController* SourceController = nullptr;
	AGDCharacterBase* SourceCharacter = nullptr;
	if (Source && Source->AbilityActorInfo.IsValid() && Source->AbilityActorInfo->AvatarActor.IsValid())
	{
		SourceActor = Source->AbilityActorInfo->AvatarActor.Get();
		SourceController = Source->AbilityActorInfo->PlayerController.Get();
		if (SourceController == nullptr && SourceActor != nullptr)
		{
			if (APawn* Pawn = Cast<APawn>(SourceActor))
			{
				SourceController = Pawn->GetController();
			}
		}

		// Use the controller to find the source pawn
		if (SourceController)
		{
			SourceCharacter = Cast<AGDCharacterBase>(SourceController->GetPawn());
		}
		else
		{
			SourceCharacter = Cast<AGDCharacterBase>(SourceActor);
		}

		// Set the causer actor based on context if it's set
		if (Context.GetEffectCauser())
		{
			SourceActor = Context.GetEffectCauser();
		}
	}

	if (Data.EvaluatedData.Attribute == GetDamageAttribute())
	{
		// Try to extract a hit result
		FHitResult HitResult;
		if (Context.GetHitResult())
		{
			HitResult = *Context.GetHitResult();
		}

		// Store a local copy of the amount of damage done and clear the damage attribute
		const float LocalDamageDone = GetDamage();
		SetDamage(0.f);
	
		if (LocalDamageDone > 0.0f)
		{
			// If character was alive before damage is added, handle damage
			// This prevents damage being added to dead things and replaying death animations
			bool WasAlive = true;

			if (TargetCharacter)
			{
				WasAlive = TargetCharacter->IsAlive();
			}

			if (!TargetCharacter->IsAlive())
			{
				//UE_LOG(LogTemp, Warning, TEXT("%s() %s is NOT alive when receiving damage"), TEXT(__FUNCTION__), *TargetCharacter->GetName());
			}

			// Apply the health change and then clamp it
			const float NewHealth = GetHealth() - LocalDamageDone;
			SetHealth(FMath::Clamp(NewHealth, 0.0f, GetMaxHealth()));

			if (TargetCharacter && WasAlive)
			{
				// This is the log statement for damage received. Turned off for live games.
				//UE_LOG(LogTemp, Log, TEXT("%s() %s Damage Received: %f"), TEXT(__FUNCTION__), *GetOwningActor()->GetName(), LocalDamageDone);

				// Play HitReact animation and sound with a multicast RPC.
				const FHitResult* Hit = Data.EffectSpec.GetContext().GetHitResult();

				if (Hit)
				{
					EGDHitReactDirection HitDirection = TargetCharacter->GetHitReactDirection(Data.EffectSpec.GetContext().GetHitResult()->Location);
					switch (HitDirection)
					{
					case EGDHitReactDirection::Left:
						TargetCharacter->PlayHitReact(HitDirectionLeftTag, SourceCharacter);
						break;
					case EGDHitReactDirection::Front:
						TargetCharacter->PlayHitReact(HitDirectionFrontTag, SourceCharacter);
						break;
					case EGDHitReactDirection::Right:
						TargetCharacter->PlayHitReact(HitDirectionRightTag, SourceCharacter);
						break;
					case EGDHitReactDirection::Back:
						TargetCharacter->PlayHitReact(HitDirectionBackTag, SourceCharacter);
						break;
					}
				}
				else
				{
					// No hit result. Default to front.
					TargetCharacter->PlayHitReact(HitDirectionFrontTag, SourceCharacter);
				}

				// Show damage number for the Source player unless it was self damage
				if (SourceActor != TargetActor)
				{
					AGDPlayerController* PC = Cast<AGDPlayerController>(SourceController);
					if (PC)
					{
						PC->ShowDamageNumber(LocalDamageDone, TargetCharacter);
					}
				}

				if (!TargetCharacter->IsAlive())
				{
					// TargetCharacter was alive before this damage and now is not alive, give XP and Gold bounties to Source.
					// Don't give bounty to self.
					if (SourceController != TargetController)
					{
						// Create a dynamic instant Gameplay Effect to give the bounties
						UGameplayEffect* GEBounty = NewObject<UGameplayEffect>(GetTransientPackage(), FName(TEXT("Bounty")));
						GEBounty->DurationPolicy = EGameplayEffectDurationType::Instant;

						int32 Idx = GEBounty->Modifiers.Num();
						GEBounty->Modifiers.SetNum(Idx + 2);

						FGameplayModifierInfo& InfoXP = GEBounty->Modifiers[Idx];
						InfoXP.ModifierMagnitude = FScalableFloat(GetXPBounty());
						InfoXP.ModifierOp = EGameplayModOp::Additive;
						InfoXP.Attribute = UGDAttributeSetBase::GetXPAttribute();

						FGameplayModifierInfo& InfoGold = GEBounty->Modifiers[Idx + 1];
						InfoGold.ModifierMagnitude = FScalableFloat(GetGoldBounty());
						InfoGold.ModifierOp = EGameplayModOp::Additive;
						InfoGold.Attribute = UGDAttributeSetBase::GetGoldAttribute();

						Source->ApplyGameplayEffectToSelf(GEBounty, 1.0f, Source->MakeEffectContext());
					}
				}
			}
		}
	}// Damage
	else if (Data.EvaluatedData.Attribute == GetHealthAttribute())
	{
		// Handle other health changes.
		// Health loss should go through Damage.
		SetHealth(FMath::Clamp(GetHealth(), 0.0f, GetMaxHealth()));
	} // Health
	else if (Data.EvaluatedData.Attribute == GetManaAttribute())
	{
		// Handle mana changes.
		SetMana(FMath::Clamp(GetMana(), 0.0f, GetMaxMana()));
	} // Mana
	else if (Data.EvaluatedData.Attribute == GetStaminaAttribute())
	{
		// Handle stamina changes.
		SetStamina(FMath::Clamp(GetStamina(), 0.0f, GetMaxStamina()));
	}
}

void UGDAttributeSetBase::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UGDAttributeSetBase, Health);
	DOREPLIFETIME(UGDAttributeSetBase, MaxHealth);
	DOREPLIFETIME(UGDAttributeSetBase, HealthRegenRate);
	DOREPLIFETIME(UGDAttributeSetBase, Mana);
	DOREPLIFETIME(UGDAttributeSetBase, MaxMana);
	DOREPLIFETIME(UGDAttributeSetBase, ManaRegenRate);
	DOREPLIFETIME(UGDAttributeSetBase, Stamina);
	DOREPLIFETIME(UGDAttributeSetBase, MaxStamina);
	DOREPLIFETIME(UGDAttributeSetBase, StaminaRegenRate);
	DOREPLIFETIME(UGDAttributeSetBase, Armor);
	DOREPLIFETIME(UGDAttributeSetBase, MoveSpeed);
	DOREPLIFETIME(UGDAttributeSetBase, CharacterLevel);
	DOREPLIFETIME(UGDAttributeSetBase, XP);
	DOREPLIFETIME(UGDAttributeSetBase, XPBounty);
	DOREPLIFETIME(UGDAttributeSetBase, Gold);
	DOREPLIFETIME(UGDAttributeSetBase, GoldBounty);
}

void UGDAttributeSetBase::AdjustAttributeForMaxChange(FGameplayAttributeData & AffectedAttribute, const FGameplayAttributeData & MaxAttribute, float NewMaxValue, const FGameplayAttribute & AffectedAttributeProperty)
{
	UAbilitySystemComponent* AbilityComp = GetOwningAbilitySystemComponent();
	const float CurrentMaxValue = MaxAttribute.GetCurrentValue();
	if (!FMath::IsNearlyEqual(CurrentMaxValue, NewMaxValue) && AbilityComp)
	{
		// Change current value to maintain the current Val / Max percent
		const float CurrentValue = AffectedAttribute.GetCurrentValue();
		float NewDelta = (CurrentMaxValue > 0.f) ? (CurrentValue * NewMaxValue / CurrentMaxValue) - CurrentValue : NewMaxValue;

		AbilityComp->ApplyModToAttributeUnsafe(AffectedAttributeProperty, EGameplayModOp::Additive, NewDelta);
	}
}

void UGDAttributeSetBase::OnRep_Health()
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGDAttributeSetBase, Health);
}

void UGDAttributeSetBase::OnRep_MaxHealth()
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGDAttributeSetBase, MaxHealth);
}

void UGDAttributeSetBase::OnRep_HealthRegenRate()
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGDAttributeSetBase, HealthRegenRate);
}

void UGDAttributeSetBase::OnRep_Mana()
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGDAttributeSetBase, Mana);
}

void UGDAttributeSetBase::OnRep_MaxMana()
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGDAttributeSetBase, MaxMana);
}

void UGDAttributeSetBase::OnRep_ManaRegenRate()
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGDAttributeSetBase, ManaRegenRate);
}

void UGDAttributeSetBase::OnRep_Stamina()
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGDAttributeSetBase, Stamina);
}

void UGDAttributeSetBase::OnRep_MaxStamina()
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGDAttributeSetBase, MaxStamina);
}

void UGDAttributeSetBase::OnRep_StaminaRegenRate()
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGDAttributeSetBase, StaminaRegenRate);
}

void UGDAttributeSetBase::OnRep_Armor()
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGDAttributeSetBase, Armor);
}

void UGDAttributeSetBase::OnRep_MoveSpeed()
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGDAttributeSetBase, MoveSpeed);
}

void UGDAttributeSetBase::OnRep_CharacterLevel()
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGDAttributeSetBase, CharacterLevel);
}

void UGDAttributeSetBase::OnRep_XP()
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGDAttributeSetBase, XP);
}

void UGDAttributeSetBase::OnRep_XPBounty()
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGDAttributeSetBase, XPBounty);
}

void UGDAttributeSetBase::OnRep_Gold()
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGDAttributeSetBase, Gold);
}

void UGDAttributeSetBase::OnRep_GoldBounty()
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGDAttributeSetBase, GoldBounty);
}
