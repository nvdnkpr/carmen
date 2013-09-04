// v8
#include <v8.h>

// node
#include <node.h>
#include <node_version.h>
#include <node_object_wrap.h>
#include <node_buffer.h>

// stl
#include <iostream>
#include <exception>
#include <string>

#include "pbf.hpp"
#include "nan.h"
#include "index.capnp.h"
#include "index.pb.h"
#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include "capnproto_helper.hpp"

// https://github.com/jasondelponte/go-v8/blob/master/src/v8context.cc#L41
// http://v8.googlecode.com/svn/trunk/test/cctest/test-threads.cc

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>

namespace node_mem {

using namespace v8;

typedef std::vector<std::vector<uint64_t>> varray;
typedef std::map<uint64_t,varray> arraycache;
typedef arraycache::const_iterator arraycache_iterator;
typedef std::map<std::string,arraycache> memcache;
typedef memcache::const_iterator mem_iterator_type;

class Cache: public node::ObjectWrap {
public:
    static Persistent<FunctionTemplate> constructor;
    static void Initialize(Handle<Object> target);
    static NAN_METHOD(New);
    static NAN_METHOD(has);
    static NAN_METHOD(load);
    static NAN_METHOD(loadJSON);
    static NAN_METHOD(search);
    static NAN_METHOD(pack);
    static NAN_METHOD(list);
    static NAN_METHOD(set);
    static void AsyncRun(uv_work_t* req);
    static void AfterRun(uv_work_t* req);
    Cache(std::string const& id, int shardlevel);
    void _ref() { Ref(); }
    void _unref() { Unref(); }
private:
    ~Cache();
    std::string id_;
    int shardlevel_;
    memcache cache_;
};

Persistent<FunctionTemplate> Cache::constructor;

void Cache::Initialize(Handle<Object> target) {
    NanScope();
    Local<FunctionTemplate> t = FunctionTemplate::New(Cache::New);
    t->InstanceTemplate()->SetInternalFieldCount(1);
    t->SetClassName(String::NewSymbol("Cache"));
    NODE_SET_PROTOTYPE_METHOD(t, "has", has);
    NODE_SET_PROTOTYPE_METHOD(t, "load", load);
    NODE_SET_PROTOTYPE_METHOD(t, "loadJSON", loadJSON);
    NODE_SET_PROTOTYPE_METHOD(t, "search", search);
    NODE_SET_PROTOTYPE_METHOD(t, "pack", pack);
    NODE_SET_PROTOTYPE_METHOD(t, "list", list);
    NODE_SET_PROTOTYPE_METHOD(t, "_set", set);
    target->Set(String::NewSymbol("Cache"),t->GetFunction());
    NanAssignPersistent(FunctionTemplate, constructor, t);
}

Cache::Cache(std::string const& id, int shardlevel)
  : ObjectWrap(),
    id_(id),
    shardlevel_(shardlevel),
    cache_()
    { }

Cache::~Cache() { }

NAN_METHOD(Cache::pack)
{
    NanScope();
    if (args.Length() < 2) {
        return NanThrowTypeError("expected three args: 'type','shard','encoding'");
    }
    if (!args[0]->IsString()) {
        return NanThrowTypeError("first argument must be a string");
    }
    if (!args[1]->IsNumber()) {
        return NanThrowTypeError("second arg must be an integer");
    }
    std::string encoding("capnproto");
    if (args.Length() > 2) {
        if (!args[2]->IsString()) {
            return NanThrowTypeError("third arg must be a string");
        }
        encoding = *String::Utf8Value(args[2]->ToString());
        if (encoding != "protobuf" && encoding != "capnproto") {
            return NanThrowTypeError((std::string("invalid encoding: ")+ encoding).c_str());
        }
    }
        try {
        std::string type = *String::Utf8Value(args[0]->ToString());
        std::string shard = *String::Utf8Value(args[1]->ToString());
        std::string key = type + "-" + shard;
        Cache* c = node::ObjectWrap::Unwrap<Cache>(args.This());
        memcache const& mem = c->cache_;
        mem_iterator_type itr = mem.find(key);
        if (itr != mem.end()) {
            arraycache_iterator aitr = itr->second.begin();
            arraycache_iterator aend = itr->second.end();
            unsigned idx = 0;
            if (encoding == "capnproto") {
                uint firstSegmentWords = 1024*1024*1024;
                ::capnp::AllocationStrategy allocationStrategy = ::capnp::SUGGESTED_ALLOCATION_STRATEGY;
                ::capnp::MallocMessageBuilder message(firstSegmentWords,allocationStrategy);
                carmen::Message::Builder msg = message.initRoot<carmen::Message>();
                ::capnp::List<carmen::Item>::Builder items = msg.initItems(itr->second.size());
                while (aitr != aend) {
                    carmen::Item::Builder item = items[idx++];
                    item.setKey(aitr->first);
                    varray const & varr = aitr->second;
                    unsigned arr_len = varr.size();
                    auto arrays = item.initArrays(arr_len);
                    for (unsigned i=0;i<arr_len;++i) {
                        carmen::Array::Builder arr2 = arrays[i];
                        std::vector<uint64_t> const& vals = varr[i];
                        unsigned val_len = vals.size();
                        auto val = arr2.initVal(val_len);
                        for (unsigned j=0;j<val_len;++j) {
                            val.set(j,vals[j]);
                        }
                    }
                    ++aitr;
                }
                TestPipe pipe;
                capnp::writePackedMessage(pipe, message);
                NanReturnValue(node::Buffer::New(pipe.getData().data(),pipe.getData().size())->handle_);
            } else {
                carmen::proto::object msg;
                while (aitr != aend) {
                    ::carmen::proto::object_item * item = msg.add_items(); 
                    item->set_key(aitr->first);
                    varray const & varr = aitr->second;
                    unsigned varr_size = varr.size();
                    item->set_size(varr_size);
                    for (unsigned i=0;i<varr_size;++i) {
                        ::carmen::proto::object_array * array = item->add_arrays();
                        std::vector<uint64_t> const& vals = varr[i];
                        for (unsigned j=0;j<vals.size();++j) {
                            array->add_val(vals[j]);
                        }
                    }
                    ++aitr;
                }
                int size = msg.ByteSize();
        #if NODE_VERSION_AT_LEAST(0, 11, 0)
                Local<Object> retbuf = node::Buffer::New(size);
                if (msg.SerializeToArray(node::Buffer::Data(retbuf),size))
                {
                    NanReturnValue(retbuf);
                }
        #else
                node::Buffer *retbuf = node::Buffer::New(size);
                if (msg.SerializeToArray(node::Buffer::Data(retbuf),size))
                {
                    NanReturnValue(retbuf->handle_);
                }
        #endif
            }
        }
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }

    NanReturnValue(Undefined());
}

NAN_METHOD(Cache::list)
{
    NanScope();
    if (args.Length() < 1) {
        return NanThrowTypeError("expected at least one arg: 'type' + optional 'shard'");
    }
    if (!args[0]->IsString()) {
        return NanThrowTypeError("first argument must be a string");
    }
    try {
        std::string type = *String::Utf8Value(args[0]->ToString());
        Cache* c = node::ObjectWrap::Unwrap<Cache>(args.This());
        memcache & mem = c->cache_;
        Local<Array> ids = Array::New();
        if (args.Length() == 1) {
            mem_iterator_type itr = mem.begin();
            mem_iterator_type end = mem.end();
            unsigned idx = 0;
            while (itr != end) {
                if (itr->first.size() > type.size() && itr->first.substr(0,type.size()) == type) {
                    std::string shard = itr->first.substr(type.size()+1,itr->first.size());
                    ids->Set(idx++,Number::New(String::New(shard.c_str())->NumberValue()));
                }
                ++itr;
            }
            NanReturnValue(ids);
        } else if (args.Length() == 2) {
            std::string shard = *String::Utf8Value(args[1]->ToString());
            std::string key = type + "-" + shard;
            mem_iterator_type itr = mem.find(key);
            if (itr != mem.end()) {
                arraycache_iterator aitr = itr->second.begin();
                arraycache_iterator aend = itr->second.end();
                unsigned idx = 0;
                while (aitr != aend) {
                    ids->Set(idx++,Number::New(aitr->first)->ToString());
                    ++aitr;
                }
                NanReturnValue(ids);
            }
        }
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
    NanReturnValue(Undefined());
}

NAN_METHOD(Cache::set)
{
    NanScope();
    if (args.Length() < 3) {
        return NanThrowTypeError("expected three args: 'type','shard','id','data'");
    }
    if (!args[0]->IsString()) {
        return NanThrowTypeError("first argument must be a string");
    }
    if (!args[1]->IsNumber()) {
        return NanThrowTypeError("second arg must be an integer");
    }
    if (!args[2]->IsNumber()) {
        return NanThrowTypeError("third arg must be an integer");
    }
    if (!args[3]->IsArray()) {
        return NanThrowTypeError("fourth arg must be an array");
    }
    Local<Array> data = Local<Array>::Cast(args[3]);
    if (data->IsNull() || data->IsUndefined()) {
        return NanThrowTypeError("an array expected for third argument");
    }
    try {
        std::string type = *String::Utf8Value(args[0]->ToString());
        std::string shard = *String::Utf8Value(args[1]->ToString());
        std::string key = type + "-" + shard;
        Cache* c = node::ObjectWrap::Unwrap<Cache>(args.This());
        memcache & mem = c->cache_;
        mem_iterator_type itr = mem.find(key);
        if (itr == mem.end()) {
            c->cache_.emplace(key,arraycache());    
        }
        arraycache & arrc = c->cache_[key];
        uint64_t key_id = args[2]->NumberValue();
        arraycache_iterator itr2 = arrc.find(key_id);
        if (itr2 == arrc.end()) {
            arrc.emplace(key_id,varray());   
        }
        varray & vv = arrc[key_id];
        if (itr2 != arrc.end()) {
            vv.clear();
        }
        unsigned array_size = data->Length();
        if (type == "grid") {
            vv.reserve(array_size);
            for (unsigned i=0;i<array_size;++i) {
                vv.emplace_back(std::vector<uint64_t>());
                std::vector<uint64_t> & vvals = vv.back();
                Local<Array> subarray = Local<Array>::Cast(data->Get(i));
                unsigned vals_size = subarray->Length();
                vvals.reserve(vals_size);
                for (unsigned k=0;k<vals_size;++k) {
                    vvals.emplace_back(subarray->Get(k)->NumberValue());
                }
            }
        } else {
            vv.reserve(1);
            vv.emplace_back(std::vector<uint64_t>());
            std::vector<uint64_t> & vvals = vv.back();
            for (unsigned i=0;i<array_size;++i) {
                vvals.emplace_back(data->Get(i)->NumberValue());
            }
        }
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
    NanReturnValue(Undefined());
}

NAN_METHOD(Cache::loadJSON)
{
    NanScope();
    if (args.Length() < 3) {
        return NanThrowTypeError("expected four args: 'object','type','shard'");
    }
    if (!args[0]->IsObject()) {
        return NanThrowTypeError("first argument must be an object");
    }
    Local<Object> obj = args[0]->ToObject();
    if (obj->IsNull() || obj->IsUndefined()) {
        return NanThrowTypeError("a valid object expected for first argument");
    }
    if (!args[1]->IsString()) {
        return NanThrowTypeError("second arg 'type' must be a string");
    }
    if (!args[2]->IsNumber()) {
        return NanThrowTypeError("third arg 'shard' must be an Integer");
    }
    try {
        std::string type = *String::Utf8Value(args[1]->ToString());
        std::string shard = *String::Utf8Value(args[2]->ToString());
        std::string key = type + "-" + shard;
        Cache* c = node::ObjectWrap::Unwrap<Cache>(args.This());
        memcache & mem = c->cache_;
        mem_iterator_type itr = mem.find(key);
        if (itr == mem.end()) {
            c->cache_.emplace(key,arraycache());    
        }
        arraycache & arrc = c->cache_[key];
        v8::Local<v8::Array> propertyNames = obj->GetPropertyNames();
        uint32_t prop_len = propertyNames->Length();
        for (uint32_t i=0;i < prop_len;++i) {
            v8::Local<v8::Value> key = propertyNames->Get(i);
            v8::Local<v8::Value> prop = obj->Get(key);
            if (prop->IsArray()) {
                uint64_t key_id = key->NumberValue();
                arrc.emplace(key_id,varray());
                varray & vv = arrc[key_id];
                v8::Local<v8::Array> arr = v8::Local<v8::Array>::Cast(prop);
                if (type == "grid") {
                    uint32_t arr_len = arr->Length();
                    vv.reserve(arr_len);
                    for (uint32_t j=0;j < arr_len;++j) {
                        v8::Local<v8::Value> val_array = arr->Get(j);
                        if (val_array->IsArray()) {
                            vv.emplace_back(std::vector<uint64_t>());
                            std::vector<uint64_t> & vvals = vv.back();
                            v8::Local<v8::Array> vals = v8::Local<v8::Array>::Cast(val_array);
                            uint32_t val_len = vals->Length();
                            vvals.reserve(val_len);
                            for (uint32_t k=0;k < val_len;++k) {
                                vvals.emplace_back(vals->Get(k)->NumberValue());
                            }
                        }
                    }
                } else {
                    uint32_t arr_len = arr->Length();
                    vv.reserve(1);
                    vv.emplace_back(std::vector<uint64_t>());
                    std::vector<uint64_t> & vvals = vv.back();
                    vvals.reserve(arr_len);
                    for (uint32_t j=0;j < arr_len;++j) {
                        v8::Local<v8::Value> val = arr->Get(j);
                        if (val->IsNumber()) {
                            vvals.emplace_back(val->NumberValue());
                        }
                    }
                }
            }
        }
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
    NanReturnValue(Undefined());
}

NAN_METHOD(Cache::load)
{
    NanScope();
    if (args.Length() < 3) {
        return NanThrowTypeError("expected four args: 'buffer','type','shard','encoding'");
    }
    if (!args[0]->IsObject()) {
        return NanThrowTypeError("first argument must be a buffer");
    }
    Local<Object> obj = args[0]->ToObject();
    if (obj->IsNull() || obj->IsUndefined()) {
        return NanThrowTypeError("a buffer expected for first argument");
    }
    if (!node::Buffer::HasInstance(obj)) {
        return NanThrowTypeError("first argument must be a buffer");
    }
    if (!args[1]->IsString()) {
        return NanThrowTypeError("second arg 'type' must be a string");
    }
    if (!args[2]->IsNumber()) {
        return NanThrowTypeError("third arg 'shard' must be an Integer");
    }
    try {
        std::string encoding("capnproto");
        if (args.Length() > 3) {
            if (!args[3]->IsString()) {
                return NanThrowTypeError("third arg must be a string");
            }
            encoding = *String::Utf8Value(args[3]->ToString());
            if (encoding != "protobuf" && encoding != "capnproto") {
                return NanThrowTypeError((std::string("invalid encoding: ")+ encoding).c_str());
            }
        }
        const char * cdata = node::Buffer::Data(obj);
        size_t size = node::Buffer::Length(obj);
        std::string type = *String::Utf8Value(args[1]->ToString());
        std::string shard = *String::Utf8Value(args[2]->ToString());
        std::string key = type + "-" + shard;
        Cache* c = node::ObjectWrap::Unwrap<Cache>(args.This());
        memcache & mem = c->cache_;
        mem_iterator_type itr = mem.find(key);
        if (itr == mem.end()) {
            c->cache_.emplace(key,arraycache());    
        }
        arraycache & arrc = c->cache_[key];
        if (encoding == "capnproto") {
            BufferStream pipe(cdata,size);
            ::capnp::PackedMessageReader reader(pipe);
            auto msg = reader.getRoot<carmen::Message>();
            auto items = msg.getItems();
            unsigned items_size = items.size();
            for (unsigned i=0;i<items_size;++i) {
                auto item = items[i];
                auto array = item.getArrays();
                unsigned array_size = array.size();
                uint64_t key_id = item.getKey();
                #if 1
                varray vv(array_size);
                for (unsigned j=0;j<array_size;++j) {
                    auto arr = array[j];
                    auto vals = arr.getVal();
                    unsigned vals_size = vals.size();
                    std::vector<uint64_t> vvals(vals_size);
                    for (unsigned k=0;k<vals_size;++k) {
                        vvals[k] = vals[k];
                    }
                    vv[j] = std::move(vvals);
                }
                arrc[key_id] = std::move(vv);
                #else
                arrc.emplace(key_id,varray());;
                varray & vv = arrc[key_id];
                vv.reserve(array_size);
                for (unsigned j=0;j<array_size;++j) {
                    auto arr = array[j];
                    auto vals = arr.getVal();
                    unsigned vals_size = vals.size();
                    vv.emplace_back(std::vector<uint64_t>());
                    std::vector<uint64_t> & vvals = vv.back();
                    vvals.reserve(vals_size);
                    for (unsigned k=0;k<vals_size;++k) {
                        vvals.emplace_back(vals[k]);
                    }
                }
                #endif
            }
        } else {
            llmr::pbf message(cdata,size);
            while (message.next()) {
                if (message.tag == 1) {
                    uint32_t bytes = message.varint();
                    llmr::pbf item(message.data, bytes);
                    uint64_t key_id = 0;
                    while (item.next()) {
                        if (item.tag == 1) {
                            key_id = item.varint();
                            arrc.emplace(key_id,varray());
                        } else if (item.tag == 2) {
                            if (key_id == 0) throw std::runtime_error("key_id not initialized!");
                            varray & vv = arrc[key_id];
                            uint32_t arrays_length = item.varint();
                            llmr::pbf array(item.data,arrays_length);
                            while (array.next()) {
                                if (array.tag == 1) {
                                    vv.emplace_back(std::vector<uint64_t>());
                                    std::vector<uint64_t> & vvals = vv.back();
                                    uint32_t vals_length = array.varint();
                                    llmr::pbf val(array.data,vals_length);
                                    while (val.next()) {
                                        vvals.emplace_back(val.value);
                                    }
                                    array.skipBytes(vals_length);
                                } else {
                                    throw std::runtime_error("skipping when shouldnt");
                                    array.skip();
                                }
                            }
                            item.skipBytes(arrays_length);
                        } else {
                            throw std::runtime_error("hit unknown type");
                        }
                    }
                    message.skipBytes(bytes);
                } else {
                    throw std::runtime_error("skipping when shouldnt");
                    message.skip();
                }
            }
        }
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
    NanReturnValue(Undefined());
}


NAN_METHOD(Cache::has)
{
    NanScope();
    if (args.Length() < 2) {
        return NanThrowTypeError("expected two args: type and shard");
    }
    if (!args[0]->IsString()) {
        return NanThrowTypeError("first arg must be a string");
    }
    if (!args[1]->IsNumber()) {
        return NanThrowTypeError("second arg must be an integer");
    }
    try {
        std::string type = *String::Utf8Value(args[0]->ToString());
        std::string shard = *String::Utf8Value(args[1]->ToString());
        std::string key = type + "-" + shard;
        Cache* c = node::ObjectWrap::Unwrap<Cache>(args.This());
        memcache const& mem = c->cache_;
        mem_iterator_type itr = mem.find(key);
        if (itr != mem.end()) {
            NanReturnValue(True());
        } else {
            NanReturnValue(False());
        }
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
}

NAN_METHOD(Cache::search)
{
    NanScope();
    if (args.Length() < 3) {
        return NanThrowTypeError("expected two args: type, shard, and id");
    }
    if (!args[0]->IsString()) {
        return NanThrowTypeError("first arg must be a string");
    }
    if (!args[1]->IsNumber()) {
        return NanThrowTypeError("second arg must be an integer");
    }
    if (!args[2]->IsNumber()) {
        return NanThrowTypeError("third arg must be an integer");
    }
    try {
        std::string type = *String::Utf8Value(args[0]->ToString());
        std::string shard = *String::Utf8Value(args[1]->ToString());
        uint64_t id = args[2]->NumberValue();
        std::string key = type + "-" + shard;
        Cache* c = node::ObjectWrap::Unwrap<Cache>(args.This());
        memcache const& mem = c->cache_;
        mem_iterator_type itr = mem.find(key);
        if (itr == mem.end()) {
            NanReturnValue(Undefined());
        } else {
            arraycache_iterator aitr = itr->second.find(id);
            if (aitr == itr->second.end()) {
                NanReturnValue(Undefined());
            } else {
                auto const& array = aitr->second;
                if (type == "grid") {
                    unsigned array_size = array.size();
                    Local<Array> arr_obj = Array::New(array_size);
                    for (unsigned j=0;j<array_size;++j) {
                        auto arr = array[j];
                        unsigned vals_size = arr.size();
                        Local<Array> vals_obj = Array::New(vals_size);
                        for (unsigned k=0;k<vals_size;++k) {
                            vals_obj->Set(k,Number::New(arr[k]));
                        }
                        arr_obj->Set(j,vals_obj);
                    }
                    NanReturnValue(arr_obj);
                } else {
                    auto arr = array[0];
                    unsigned vals_size = arr.size();
                    Local<Array> arr_obj = Array::New(vals_size);
                    for (unsigned k=0;k<vals_size;++k) {
                        arr_obj->Set(k,Number::New(arr[k]));
                    }
                    NanReturnValue(arr_obj);
                }
            }
        }
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
}

NAN_METHOD(Cache::New)
{
    NanScope();
    if (!args.IsConstructCall()) {
        return NanThrowTypeError("Cannot call constructor as function, you need to use 'new' keyword");
    }
    try {
        if (args.Length() < 2) {
            return NanThrowTypeError("expected 'id' and 'shardlevel' arguments");
        }
        if (!args[0]->IsString()) {
            return NanThrowTypeError("first argument 'id' must be a string");
        }
        if (!args[1]->IsNumber()) {
            return NanThrowTypeError("first argument 'shardlevel' must be a number");
        }
        std::string id = *String::Utf8Value(args[0]->ToString());
        int shardlevel = args[1]->IntegerValue();
        Cache* im = new Cache(id,shardlevel);
        im->Wrap(args.This());
        args.This()->Set(String::NewSymbol("id"),args[0]);
        args.This()->Set(String::NewSymbol("shardlevel"),args[1]);
        NanReturnValue(args.This());
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
    NanReturnValue(Undefined());
}

extern "C" {
    static void start(Handle<Object> target) {
        Cache::Initialize(target);
    }
}

} // namespace node_mem

NODE_MODULE(mem, node_mem::start)
