#include "RI/Topic.h"
#include "bson.h"
#include "rosbridge2cpp/ros_bridge.h"
#include "rosbridge2cpp/ros_topic.h"
#include "Conversion/Messages/BaseMessageConverter.h"
#include "Conversion/Messages/std_msgs/StdMsgsStringConverter.h"

static TMap<FString, UBaseMessageConverter*> TypeConverterMap;

// PIMPL
class UTopic::Impl {
	// hidden implementation details
public:
	Impl()
    : _Ric(nullptr)
    , _ROSTopic(nullptr)
    {
	}

    ~Impl() {

        if (_Callback && _Ric) {
            Unsubscribe();
        }

        delete _ROSTopic;
    }

	UROSIntegrationCore* _Ric;
	FString _Topic;
	FString _MessageType;
    int32 _QueueSize;
	rosbridge2cpp::ROSTopic* _ROSTopic;

	std::function<void(TSharedPtr<FROSBaseMsg>)> _Callback;

	bool ConvertMessage(TSharedPtr<FROSBaseMsg> BaseMsg, bson_t** message) {
		// TODO do this on advertise/call?
		UBaseMessageConverter** Converter = TypeConverterMap.Find(_MessageType);
		if (!Converter) {
			UE_LOG(LogROS, Error, TEXT("MessageType %s is unknown. Can't find Converter to encode message"), *_MessageType);
			return false;
		}

		return (*Converter)->ConvertOutgoingMessage(BaseMsg, message);
	}

	// IN Parameter: message
	// OUT Parameter: BaseMsg
	bool ConvertMessage(const ROSBridgePublishMsg* message, TSharedPtr<FROSBaseMsg> &BaseMsg) {
		// TODO do this on advertise/call?
		UBaseMessageConverter** Converter = TypeConverterMap.Find(_MessageType);
		if (!Converter) {
			UE_LOG(LogROS, Error, TEXT("MessageType %s is unknown. Can't find Converter to decode message"), *_MessageType);
			return false;
		}

		return (*Converter)->ConvertIncomingMessage(message, BaseMsg);
	}


	bool Subscribe(std::function<void(TSharedPtr<FROSBaseMsg>)> func) {
		if (!_ROSTopic) {
			UE_LOG(LogROS, Error, TEXT("Rostopic hasn't been initialized before Subscribe() call"));
			return false;
		}
        if (_Callback) {
            UE_LOG(LogROS, Warning, TEXT("Rostopic was already subscribed"));
            Unsubscribe();
        }

		bool result = _ROSTopic->Subscribe(std::bind(&UTopic::Impl::MessageCallback, this, std::placeholders::_1));
		_Callback = func;
        return result;
	}
    bool Unsubscribe() {
        if (!_ROSTopic) {
            UE_LOG(LogROS, Error, TEXT("Rostopic hasn't been initialized before Unsubscribe() call"));
            return false;
        }

        bool result = _ROSTopic->Unsubscribe(std::bind(&UTopic::Impl::MessageCallback, this, std::placeholders::_1));
        if (result) {
            _Callback = nullptr;
        }
        return result;
	}

	bool Advertise() {
		assert(_ROSTopic);
        return _ROSTopic->Advertise();
	}


    bool Unadvertise() {
		assert(_ROSTopic);
		return _ROSTopic->Unadvertise();
	}


	bool Publish(TSharedPtr<FROSBaseMsg> msg) {
		bson_t *bson_message = nullptr;

		if (ConvertMessage(msg, &bson_message)) {
			return _ROSTopic->Publish(bson_message);
			//bson_destroy(bson_message); // Not necessary, since bson memory will be freed in the rosbridge core code
		}
		else {
			UE_LOG(LogROS, Error, TEXT("Failed to ConvertMessage in UTopic::Publish()"));
            return false;
		}
	}

	void Init(UROSIntegrationCore *Ric, FString Topic, FString MessageType, int32 QueueSize) {
		_Ric = Ric;
		_Topic = Topic;
		_MessageType = MessageType;
        _QueueSize = QueueSize;

		_ROSTopic = new rosbridge2cpp::ROSTopic(Ric->_Implementation->_Ros, TCHAR_TO_UTF8(*Topic), TCHAR_TO_UTF8(*MessageType), QueueSize);

		// Construct ConverterMap
        if(TypeConverterMap.Num() == 0)
        { 
		    for (TObjectIterator<UClass> It; It; ++It)
		    {
			    UClass* ClassItr = *It;

			    if (It->IsChildOf(UBaseMessageConverter::StaticClass()) && *It != UBaseMessageConverter::StaticClass())
			    {
				    UBaseMessageConverter* ConcreteConverter = ClassItr->GetDefaultObject<UBaseMessageConverter>();
				    //UE_LOG(LogROS, Log, TEXT("Added %s with type %s to TopicConverterMap"), *(It->GetDefaultObjectName().ToString()), *(ConcreteConverter->_MessageType));
                    TypeConverterMap.Add(*(ConcreteConverter->_MessageType), ConcreteConverter);
			    }
		    }
        }

	}

	void MessageCallback(const ROSBridgePublishMsg &message) {
		TSharedPtr<FROSBaseMsg> BaseMsg;
		if (ConvertMessage(&message, BaseMsg)) {
			_Callback(BaseMsg);
		}
		else {
			UE_LOG(LogROS, Error, TEXT("Couldn't convert incoming Message; Skipping callback"));
		}
	}
};

// Interface Implementation

UTopic::UTopic(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
    , _Implementation(new UTopic::Impl())
    , _SelfPtr(this)
{
    _State.Advertised = false;
    _State.Subscribed = false;
    _State.Blueprint = false;
}

void UTopic::PostInitProperties()
{
    Super::PostInitProperties();

    OnConstruct();
}

void UTopic::BeginDestroy() {
    Super::BeginDestroy();

    if (!ROSIntegrationCore.Get())
    {
        _Implementation->_Ric = nullptr;
    }

	delete _Implementation;

    _SelfPtr.Reset();
}


bool UTopic::Subscribe(std::function<void(TSharedPtr<FROSBaseMsg>)> func) {
    _State.Subscribed = true;
	return _Implementation->Subscribe(func);
}
bool UTopic::Unsubscribe() {
    _State.Subscribed = false;
    return _Implementation->Unsubscribe();
}

bool UTopic::Advertise() {
    _State.Advertised = true;
    return _Implementation->Advertise();
}
bool UTopic::Unadvertise() {
    _State.Advertised = false;
    return _Implementation->Unadvertise();
}
bool UTopic::Publish(TSharedPtr<FROSBaseMsg> msg) {
    
    if (!ROSIntegrationCore.Get()) // TODO: check why this is necessary (An active Topic should always point to a valid ROSIntegrationCore)
    {
        return false;
    }

	return _Implementation->Publish(msg);
}

void UTopic::Init(UROSIntegrationCore *Ric, FString Topic, FString MessageType, int32 QueueSize) {
    ROSIntegrationCore = Ric;
	_Implementation->Init(Ric, Topic, MessageType, QueueSize);
}


bool UTopic::Reconnect(UROSIntegrationCore* ROSIntegrationCore)
{
    bool success = true;

    Impl* oldImplementation = _Implementation;
    _Implementation = new UTopic::Impl();

    this->ROSIntegrationCore = ROSIntegrationCore;

    if (_State.Subscribed && _State.Blueprint)
    {
        success = Subscribe(oldImplementation->_Topic, _State.BlueprintMessageType, oldImplementation->_QueueSize);
    }
    else if(_State.Subscribed || _State.Advertised)
    {
        _Implementation->Init(ROSIntegrationCore, oldImplementation->_Topic, oldImplementation->_MessageType, oldImplementation->_QueueSize);

        if (_State.Subscribed)
        {
            success = Subscribe(oldImplementation->_Callback);
        }
        if (_State.Advertised)
        {
            success = success && Advertise();
        }
    }

    oldImplementation->_Ric = nullptr; // prevent old topic from unsubscribing using the broken connection
    delete oldImplementation;
    return success;
}

FString UTopic::GetDetailedInfoInternal() const
{
    return _Implementation->_Topic;
}

bool UTopic::Subscribe(const FString& TopicName, EMessageType MessageType, int32 QueueSize)
{
    bool success = false;
    _State.Subscribed = true;
    _State.Blueprint = true;
    _State.BlueprintMessageType = MessageType;

    ROSIntegrationCore = nullptr;

    UROSIntegrationGameInstance* ROSInstance = Cast<UROSIntegrationGameInstance>(GWorld->GetGameInstance());
    if (ROSInstance)
    {
        if (ROSInstance->bConnectToROS)
        {
            TMap<EMessageType, FString> SupportedMessageTypes;
            SupportedMessageTypes.Add(EMessageType::String, TEXT("std_msgs/String"));
            SupportedMessageTypes.Add(EMessageType::Float32, TEXT("std_msgs/Float32"));

            Init(ROSInstance->ROSIntegrationCore, TopicName, SupportedMessageTypes[MessageType], QueueSize);

            std::function<void(TSharedPtr<FROSBaseMsg>)> Callback = [this, MessageType](TSharedPtr<FROSBaseMsg> msg) -> void
            {
                switch (MessageType)
                {
                case EMessageType::String:
                {
                    auto ConcreteStringMessage = StaticCastSharedPtr<ROSMessages::std_msgs::String>(msg);
                    if (ConcreteStringMessage.IsValid())
                    {
                        const FString Data = ConcreteStringMessage->_Data;
                        TWeakPtr<UTopic, ESPMode::ThreadSafe> SelfPtr(_SelfPtr);
                        AsyncTask(ENamedThreads::GameThread, [this, Data, SelfPtr]()
                        {
                            if (!SelfPtr.IsValid()) return;
                            OnStringMessage(Data);
                        });
                    }
                    break;
                }
                case EMessageType::Float32:
                {
                    auto ConcreteFloatMessage = StaticCastSharedPtr<ROSMessages::std_msgs::Float32>(msg);
                    if (ConcreteFloatMessage.IsValid())
                    {
                        const float Data = ConcreteFloatMessage->_Data;
                        TWeakPtr<UTopic, ESPMode::ThreadSafe> SelfPtr(_SelfPtr);
                        AsyncTask(ENamedThreads::GameThread, [this, Data, SelfPtr]()
                        {
                            if (!SelfPtr.IsValid()) return;
                            OnFloat32Message(Data);
                        });
                        
                    }
                    break;
                }
                default:
                    unimplemented();
                    break;
                }
            };

            success = Subscribe(Callback);
        }
    }
    else
    {
        UE_LOG(LogROS, Warning, TEXT("ROSIntegrationGameInstance does not exist."));
    }

    return success;
}