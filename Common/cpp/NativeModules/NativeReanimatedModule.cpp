#include "NativeReanimatedModule.h"
#include <memory>
#include "Logger.h"
#include <functional>
#include <thread>
#include "SpeedChecker.h"

using namespace facebook;

namespace facebook {
namespace react {

jsi::Value eval(jsi::Runtime &rt, const char *code) {
  return rt.global().getPropertyAsFunction(rt, "eval").call(rt, code);
}

jsi::Function function(jsi::Runtime &rt, const std::string& code) {
  return eval(rt, ("(" + code + ")").c_str()).getObject(rt).getFunction(rt);
}

NativeReanimatedModule::NativeReanimatedModule(
  std::unique_ptr<jsi::Runtime> rt,
  std::shared_ptr<ApplierRegistry> ar,
  std::shared_ptr<SharedValueRegistry> svr,
  std::shared_ptr<WorkletRegistry> wr,
  std::shared_ptr<Scheduler> scheduler,
  std::shared_ptr<MapperRegistry> mapperRegistry,
  std::shared_ptr<JSCallInvoker> jsInvoker,
  std::shared_ptr<ErrorHandler> errorHandler) : NativeReanimatedModuleSpec(jsInvoker) {

  this->applierRegistry = ar;
  this->scheduler = scheduler;
  this->workletRegistry = wr;
  this->sharedValueRegistry = svr;
  this->mapperRegistry = mapperRegistry;
  this->runtime = std::move(rt);
  this->errorHandler = errorHandler;
  this->dummyEvent = std::shared_ptr<jsi::Value>(new jsi::Value(*runtime, jsi::Value::undefined()));
}

// function install
// path can be whatever You want to use in worklets, examples:
//  Reanimated.count
//  console.log
//  a.b.c.d
// note: functions provided in `code` must be wrapped in ()
void NativeReanimatedModule::workletEval(jsi::Runtime &rt, std::string path, std::string code) {
  scheduler->scheduleOnJS([this, path, code]() {
      // create structure of objects(for those which do not exist)
      jsi::Object currentObject = this->runtime->global();
      size_t prev = 0;
      size_t curr = path.find(".");
      std::string subPath;
      while(curr != std::string::npos) {
          subPath = path.substr(prev, curr - prev);
          // for current subpath initialize with {} if does not exist
          if (currentObject.getProperty(*this->runtime, subPath.c_str()).isUndefined()) {
              currentObject.setProperty(*this->runtime, subPath.c_str(), jsi::Object(*this->runtime));
          }
          currentObject = currentObject.getProperty(*this->runtime, subPath.c_str()).asObject(*this->runtime);
          prev = curr + 1;
          curr = path.find(".", prev);
      }
      // this is the last part of the subpath - initialize it with value provided in `code`
      subPath = path.substr(prev, std::string::npos);
      std::shared_ptr<jsi::StringBuffer> buff(new jsi::StringBuffer(code));
      jsi::Value val = this->runtime->evaluateJavaScript(buff, "Native Reanimated Module");
      if (val.isUndefined() || code == "{}") {
          // if value provided in `code` could not be recognized just initialize with empty object
          val = jsi::Object(*this->runtime);
      }
      currentObject.setProperty(*this->runtime, subPath.c_str(), val);
  });
}

// worklets

void NativeReanimatedModule::registerWorklet( // make it async !!!
  jsi::Runtime &rt,
  double id,
  std::string functionAsString,
  int length) {
    scheduler->scheduleOnUI([functionAsString, id, length, this]() mutable {
    auto fun = function(*runtime, functionAsString.c_str());
    std::shared_ptr<jsi::Function> funPtr(new jsi::Function(std::move(fun)));
    this->workletRegistry->registerWorklet((int)id, funPtr, length);
  });
}

void NativeReanimatedModule::unregisterWorklet( // make it async !!!
  jsi::Runtime &rt,
  double id) {
  scheduler->scheduleOnUI([id, this]() mutable {
    this->workletRegistry->unregisterWorklet((int)id);
  });
}

void NativeReanimatedModule::setWorkletListener(jsi::Runtime &rt, int workletId, const jsi::Value &listener) {
  if (listener.isUndefined() or listener.isNull()) {
    scheduler->scheduleOnUI([this, workletId](){
      workletRegistry->setWorkletListener(workletId, std::shared_ptr<std::function<void()>>(nullptr));
    });
    return;
  }

  jsi::Function fun = listener.getObject(rt).asFunction(rt);
  std::shared_ptr<jsi::Function> funPtr(new jsi::Function(std::move(fun)));

  std::shared_ptr<std::function<void()>> wrapperFun(new std::function<void()>([this, &rt, funPtr]{
    scheduler->scheduleOnJS([&rt, funPtr]{
      funPtr->call(rt);
    });
  }));
  
  scheduler->scheduleOnUI([this, workletId, wrapperFun](){
    workletRegistry->setWorkletListener(workletId, wrapperFun);
  });
}

// SharedValue

void NativeReanimatedModule::updateSharedValueRegistry(jsi::Runtime &rt, int id, const jsi::Value &value, bool setVal) {
  std::function<std::shared_ptr<SharedValue>()> create;
  
  if (value.isNumber()) {
    double number = value.getNumber();
    create = [=] () {
      return std::shared_ptr<SharedValue>(new SharedDouble(id, number, applierRegistry, sharedValueRegistry, workletRegistry));
    };
  } else if(value.isString()) {
    std::string str = value.getString(rt).utf8(rt);
    create = [=] () {
      return std::shared_ptr<SharedValue>(new SharedString(id, str, sharedValueRegistry));
    };
  } else if(value.isObject()) {
    jsi::Object obj = value.getObject(rt);
    
    if (obj.hasProperty(rt, "isWorklet")) {
      int workletId = obj.getProperty(rt, "workletId").getNumber();
      
      std::vector<int> args;
      jsi::Array ar = obj.getProperty(rt, "argIds").getObject(rt).asArray(rt);
      for (int i = 0; i < ar.length(rt); ++i) {
        int svId = ar.getValueAtIndex(rt, i).getNumber();
        args.push_back(svId);
      }
      
      create = [=] () -> std::shared_ptr<SharedValue> {
        std::shared_ptr<Worklet> worklet = workletRegistry->getWorklet(workletId);
        if (worklet == nullptr) {
          return nullptr;
        }
        std::shared_ptr<SharedValue> sv(new SharedWorkletStarter(
            (int)id,
            worklet,
            args,
            this->sharedValueRegistry,
            this->applierRegistry));
        return sv;
      };
    }
    
    if (obj.hasProperty(rt, "isFunction")) {
      int workletId = obj.getProperty(rt, "workletId").getNumber();
      create = [=] () -> std::shared_ptr<SharedValue> {
        std::shared_ptr<Worklet> worklet = workletRegistry->getWorklet(workletId);
        if (worklet == nullptr) {
          return nullptr;
        }
        std::shared_ptr<SharedValue> sv(new SharedFunction(id, worklet));
        return sv;
      };
    };
    
    if (obj.hasProperty(rt, "isArray")) {
      std::vector<int> svIds;
      jsi::Array ar = obj.getProperty(rt, "argIds").getObject(rt).asArray(rt);
      for (int i = 0; i < ar.length(rt); ++i) {
        int svId = ar.getValueAtIndex(rt, i).getNumber();
        svIds.push_back(svId);
      }
      create = [=] () -> std::shared_ptr<SharedValue> {
        std::vector<std::shared_ptr<SharedValue>> svs;
        for (auto svId : svIds) {
          std::shared_ptr<SharedValue> sv = sharedValueRegistry->getSharedValue(svId);
          if (sv == nullptr) {
            return nullptr;
          }
          svs.push_back(sv);
        }
        std::shared_ptr<SharedValue> sv(new SharedArray(id, svs));
        return sv;
      };
    }
    
    if (obj.hasProperty(rt, "isObject")) {
      std::vector<int> svIds;
      jsi::Array ar = obj.getProperty(rt, "ids").getObject(rt).asArray(rt);
      for (int i = 0; i < ar.length(rt); ++i) {
        int svId = ar.getValueAtIndex(rt, i).getNumber();
        svIds.push_back(svId);
      }
      
      std::vector<std::string> names;
      ar = obj.getProperty(rt, "propNames").getObject(rt).asArray(rt);
      for (int i = 0; i < ar.length(rt); ++i) {
        std::string name = ar.getValueAtIndex(rt, i).getString(rt).utf8(rt);
        names.push_back(name);
      }
      
      create = [=] () -> std::shared_ptr<SharedValue> {
        std::vector<std::shared_ptr<SharedValue>> svs;
        for (auto svId : svIds) {
          std::shared_ptr<SharedValue> sv = sharedValueRegistry->getSharedValue(svId);
          if (sv == nullptr) {
            return nullptr;
          }
          svs.push_back(sv);
        }
        
        std::shared_ptr<SharedValue> sv(new SharedObject(id, svs, names));
        return sv;
      };
      
    }

  }
  
  scheduler->scheduleOnUI([=](){
    std::shared_ptr<SharedValue> oldSV = sharedValueRegistry->getSharedValue(id);
    if (oldSV != nullptr and !setVal) {
      return;
    }
    
    std::shared_ptr<SharedValue> sv = create();
    if (sv == nullptr) {
      return;
    }
    
    if (oldSV != nullptr and setVal) {
      oldSV->setNewValue(sv);
    }
    
    if (oldSV == nullptr) {
      sharedValueRegistry->registerSharedValue(id, sv);
    }
  });
}

void NativeReanimatedModule::registerSharedValue(jsi::Runtime &rt, double id, const jsi::Value &value) {
  updateSharedValueRegistry(rt, (int)id, value, false);
}

void NativeReanimatedModule::unregisterSharedValue(jsi::Runtime &rt, double id) {
  scheduler->scheduleOnUI([=](){
    sharedValueRegistry->unregisterSharedValue(id, *runtime);
  });
}

void NativeReanimatedModule::getSharedValueAsync(jsi::Runtime &rt, double id, const jsi::Value &value) {
  jsi::Function fun = value.getObject(rt).asFunction(rt);
  std::shared_ptr<jsi::Function> funPtr(new jsi::Function(std::move(fun)));

  scheduler->scheduleOnUI([&rt, id, funPtr, this]() {
    auto sv = sharedValueRegistry->getSharedValue(id);
    scheduler->scheduleOnJS([&rt, sv, funPtr] () {
      jsi::Value val = sv->asValue(rt);
      funPtr->call(rt, val);
    });
  });

}

void NativeReanimatedModule::setSharedValue(jsi::Runtime &rt, double id, const jsi::Value &value) {
  updateSharedValueRegistry(rt, (int)id, value, true);
}

void NativeReanimatedModule::registerApplierOnRender(jsi::Runtime &rt, int id, int workletId, std::vector<int> svIds) {
  scheduler->scheduleOnUI([=]() {
    std::shared_ptr<Worklet> workletPtr = workletRegistry->getWorklet(workletId);
    if (workletPtr == nullptr) {
      return;
    }
    
    std::vector<std::shared_ptr<SharedValue>> sharedValues;
    
    for (auto id : svIds) {
      std::shared_ptr<SharedValue> sv = sharedValueRegistry->getSharedValue(id);
      if (sv == nullptr) {
        return;
      }
      sharedValues.push_back(sv);
    }
    
    std::shared_ptr<Applier> applier(new Applier(id, workletPtr, sharedValues, sharedValueRegistry));
    applierRegistry->registerApplierForRender(id, applier);
  });
}

void NativeReanimatedModule::unregisterApplierFromRender(jsi::Runtime &rt, int id) {
  scheduler->scheduleOnUI([=](){
    applierRegistry->unregisterApplierFromRender(id, *runtime);
  });
}

void NativeReanimatedModule::registerApplierOnEvent(jsi::Runtime &rt, int id, std::string eventName, int workletId, std::vector<int> svIds) {
  scheduler->scheduleOnUI([=]() {
    std::shared_ptr<Worklet> workletPtr = workletRegistry->getWorklet(workletId);
    if (workletPtr == nullptr) {
      return;
    }
    
    std::vector<std::shared_ptr<SharedValue>> sharedValues;
    
    for (auto id : svIds) {
      std::shared_ptr<SharedValue> sv = sharedValueRegistry->getSharedValue(id);
      if (sv == nullptr) {
        return;
      }
      sharedValues.push_back(sv);
    }

    std::shared_ptr<Applier> applier(new Applier(id, workletPtr, sharedValues, sharedValueRegistry));
    applierRegistry->registerApplierForEvent(id, eventName, applier);
   });
}

void NativeReanimatedModule::unregisterApplierFromEvent(jsi::Runtime &rt, int id) {
  scheduler->scheduleOnUI([=](){
    applierRegistry->unregisterApplierFromEvent(id);
  });
}

void NativeReanimatedModule::registerMapper(jsi::Runtime &rt, int id, int workletId, std::vector<int> svIds) {
  scheduler->scheduleOnUI([=]() {
    std::shared_ptr<Worklet> workletPtr = workletRegistry->getWorklet(workletId);
    if (workletPtr == nullptr) {
      return;
    }

    std::vector<std::shared_ptr<SharedValue>> sharedValues;
       
    for (auto id : svIds) {
     std::shared_ptr<SharedValue> sv = sharedValueRegistry->getSharedValue(id);
     if (sv == nullptr) {
       return;
     }
     sharedValues.push_back(sv);
    }
    
    std::shared_ptr<Applier> applier(new Applier(id, workletPtr, sharedValues, sharedValueRegistry));
    std::shared_ptr<Mapper> mapper = Mapper::createMapper(id,
                                                          applier,
                                                          sharedValueRegistry);
    mapperRegistry->addMapper(mapper);
  });
}

void NativeReanimatedModule::unregisterMapper(jsi::Runtime &rt, int id) {
  scheduler->scheduleOnUI([=](){
    mapperRegistry->removeMapper(id);
  });
}

void NativeReanimatedModule::render() {

  if (this->workletModule == nullptr) {
    this->workletModule = std::shared_ptr<BaseWorkletModule>(new WorkletModule(
      sharedValueRegistry,
      applierRegistry,
      workletRegistry,
      this->dummyEvent));
  }
  SpeedChecker::checkSpeed("Render:", [=] () {
    applierRegistry->render(*runtime, this->workletModule);
  });
}

void NativeReanimatedModule::onEvent(std::string eventName, std::string eventAsString) {
  jsi::Value event = eval(*runtime, ("(" + eventAsString + ")").c_str());
  std::shared_ptr<jsi::Value> eventPtr(new jsi::Value(*runtime, event));

    
  if (this->workletModule == nullptr) {
    this->workletModule = std::shared_ptr<BaseWorkletModule>(new WorkletModule(
      sharedValueRegistry,
      applierRegistry,
      workletRegistry,
      eventPtr));
  } else {
      std::dynamic_pointer_cast<WorkletModule>(this->workletModule)->setEvent(eventPtr);
  }
  SpeedChecker::checkSpeed("Event:", [=] () { 
    applierRegistry->event(*runtime, eventName, this->workletModule);
  });
}

NativeReanimatedModule::~NativeReanimatedModule() {
  // noop
}

// test method

/*
  used for tests
*/
void NativeReanimatedModule::getRegistersState(jsi::Runtime &rt, int option, const jsi::Value &value) {
  // option:
  //  1 - shared values
  //  2 - worklets
  //  3 - appliers
  jsi::Function fun = value.getObject(rt).asFunction(rt);
  std::shared_ptr<jsi::Function> funPtr(new jsi::Function(std::move(fun)));

  scheduler->scheduleOnUI([&rt, funPtr, this, option]() {
    std::string ids;
    switch(option) {
      case 1: {
        for(auto &it : sharedValueRegistry->getSharedValueMap()) {
          ids += std::to_string(it.first) + " ";
        }
          ;
      }
      case 2: {
        for(auto it : workletRegistry->getWorkletMap()) {
          ids += std::to_string(it.first) + " ";
        }
        break;
      }
      case 3: {
        for(auto &it : applierRegistry->getRenderAppliers()) {
          ids += std::to_string(it.first) + " ";
        }
        for(auto &it : applierRegistry->getEventMapping()) {
          ids += std::to_string(it.first) + " ";
        }
        break;
      }
      default: {
        ids = "error: registers state invalid option provided ";
      }
    }
    if (ids.size() > 0) {
      ids.pop_back();
    }
    scheduler->scheduleOnJS([&rt, ids, funPtr] () {
      funPtr->call(rt, ids.c_str());
    });
  });
}

}
}