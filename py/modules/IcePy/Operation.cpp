// **********************************************************************
//
// Copyright (c) 2003-2004 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************

#include <Operation.h>
#include <Current.h>
#include <Proxy.h>
#include <Types.h>
#include <Util.h>
#include <Ice/Communicator.h>
#include <Ice/IdentityUtil.h>
#include <Ice/Initialize.h>
#include <Ice/LocalException.h>
#include <Ice/ObjectAdapter.h>
#include <Ice/Proxy.h>
#include <Slice/PythonUtil.h>

using namespace std;
using namespace IcePy;
using namespace Slice::Python;

namespace IcePy
{

class ParamInfo : public UnmarshalCallback
{
public:

    virtual void unmarshaled(PyObject*, PyObject*, void*);

    TypeInfoPtr type;
};
typedef IceUtil::Handle<ParamInfo> ParamInfoPtr;
typedef std::vector<ParamInfoPtr> ParamInfoList;

class OperationI : public Operation
{
public:

    virtual PyObject* invoke(const Ice::ObjectPrx&, const Ice::CommunicatorPtr&, PyObject*);

    virtual bool dispatch(PyObject*, const std::vector<Ice::Byte>&, std::vector<Ice::Byte>&, const Ice::Current&);

    std::string name;
    Ice::OperationMode mode;
    ParamInfoList inParams;
    ParamInfoList outParams;
    ParamInfoPtr returnType;
    ExceptionInfoList exceptions;

private:

    bool checkDispatchException(std::vector<Ice::Byte>&, const Ice::CommunicatorPtr&);
    void unmarshalException(const std::vector<Ice::Byte>&, const Ice::CommunicatorPtr&);
    bool validateException(PyObject*) const;
};
typedef IceUtil::Handle<OperationI> OperationIPtr;

struct OperationObject
{
    PyObject_HEAD
    OperationPtr* op;
};

extern PyTypeObject OperationType;

}

//
// Operation implementation.
//
IcePy::Operation::~Operation()
{
}

//
// ParamInfo implementation.
//
void
IcePy::ParamInfo::unmarshaled(PyObject* val, PyObject* target, void* closure)
{
    assert(PyTuple_Check(target));
    int i = reinterpret_cast<int>(closure);
    PyTuple_SET_ITEM(target, i, val);
    Py_INCREF(val); // PyTuple_SET_ITEM steals a reference.
}

//
// OperationI implementation.
//
PyObject*
IcePy::OperationI::invoke(const Ice::ObjectPrx& proxy, const Ice::CommunicatorPtr& communicator, PyObject* args)
{
    assert(PyTuple_Check(args));

    //
    // Validate the number of arguments. There may be an extra argument for the context.
    //
    int argc = PyTuple_GET_SIZE(args);
    int paramCount = static_cast<int>(inParams.size());
    if(argc != paramCount && argc != paramCount + 1)
    {
        string fixedName = fixIdent(name);
        PyErr_Format(PyExc_RuntimeError, "%s expects %d in parameters", fixedName.c_str(), paramCount);
        return NULL;
    }

    //
    // Retrieve the context if any.
    //
    Ice::Context ctx;
    bool haveContext = false;
    if(argc == paramCount + 1)
    {
        PyObject* pyctx = PyTuple_GET_ITEM(args, argc - 1);
        if(pyctx != Py_None)
        {
            if(!PyDict_Check(pyctx))
            {
                PyErr_Format(PyExc_ValueError, "context argument must be a dictionary");
                return NULL;
            }

            if(!dictionaryToContext(pyctx, ctx))
            {
                return NULL;
            }

            haveContext = true;
        }
    }

    try
    {
        //
        // Marshal the in parameters.
        //
        Ice::ByteSeq params;
        Ice::OutputStreamPtr os = Ice::createOutputStream(communicator);

        ObjectMap objectMap;
        int i = 0;
        ParamInfoList::iterator p;
        for(p = inParams.begin(); p != inParams.end(); ++p, ++i)
        {
            PyObject* arg = PyTuple_GET_ITEM(args, i);
            if(!(*p)->type->validate(arg))
            {
                PyErr_Format(PyExc_ValueError, "invalid value for argument %d in operation `%s'", i + 1,
                             const_cast<char*>(name.c_str()));
                return false;
            }
            (*p)->type->marshal(arg, os, &objectMap);
        }

        os->finished(params);

        //
        // Invoke the operation. Use _info->name here, not fixedName.
        //
        Ice::ByteSeq result;
        bool status;
        {
            AllowThreads allowThreads; // Release Python's global interpreter lock during remote invocations.

            if(haveContext)
            {
                status = proxy->ice_invoke(name, (Ice::OperationMode)mode, params, result, ctx);
            }
            else
            {
                status = proxy->ice_invoke(name, (Ice::OperationMode)mode, params, result);
            }
        }

        //
        // Process the reply.
        //
        if(proxy->ice_isTwoway())
        {
            if(!status)
            {
                //
                // Unmarshal and "throw" a user exception.
                //
                unmarshalException(result, communicator);
                return NULL;
            }
            else if(outParams.size() > 0 || returnType)
            {
                i = returnType ? 1 : 0;
                int numResults = static_cast<int>(outParams.size()) + i;
                PyObjectHandle results = PyTuple_New(numResults);
                if(results.get() == NULL)
                {
                    return NULL;
                }

                //
                // Unmarshal the results. If there is more than one value to be returned, then return them
                // in a tuple of the form (result, outParam1, ...). Otherwise just return the value.
                //
                Ice::InputStreamPtr is = Ice::createInputStream(communicator, result);
                for(p = outParams.begin(); p != outParams.end(); ++p, ++i)
                {
                    (*p)->type->unmarshal(is, *p, results.get(), (void*)i);
                }

                if(returnType)
                {
                    returnType->type->unmarshal(is, returnType, results.get(), 0);
                }

                is->finished();

                if(numResults > 1)
                {
                    return results.release();
                }
                else
                {
                    PyObject* ret = PyTuple_GET_ITEM(results.get(), 0);
                    Py_INCREF(ret);
                    return ret;
                }
            }
        }
    }
    catch(const AbortMarshaling&)
    {
        return NULL;
    }
    catch(const Ice::Exception& ex)
    {
        setPythonException(ex);
        return NULL;
    }

    Py_INCREF(Py_None);
    return Py_None;
}

bool
IcePy::OperationI::dispatch(PyObject* servant, const std::vector<Ice::Byte>& inBytes,
                            std::vector<Ice::Byte>& outBytes, const Ice::Current& current)
{
    string fixedName = fixIdent(current.operation);
    Ice::CommunicatorPtr communicator = current.adapter->getCommunicator();

    //
    // Unmarshal the in parameters.
    //
    int count = static_cast<int>(inParams.size());
    PyObjectHandle args = PyTuple_New(count + 1); // Leave room for a trailing Ice::Current object.
    if(args.get() == NULL)
    {
        throwPythonException();
    }

    Ice::InputStreamPtr is = Ice::createInputStream(communicator, inBytes);

    try
    {
        int i = 0;
        for(ParamInfoList::iterator p = inParams.begin(); p != inParams.end(); ++p, ++i)
        {
            (*p)->type->unmarshal(is, *p, args.get(), (void*)i);
        }
        is->finished();
    }
    catch(const AbortMarshaling&)
    {
        throwPythonException();
    }

    //
    // Create an object to represent Ice::Current. We need to append this to the argument tuple.
    //
    PyObjectHandle curr = createCurrent(current);
    if(PyTuple_SET_ITEM(args.get(), PyTuple_GET_SIZE(args.get()) - 1, curr.get()) < 0)
    {
        throwPythonException();
    }
    curr.release(); // PyTuple_SET_ITEM steals a reference.

    //
    // Dispatch the operation. Use fixedName here, not current.operation.
    //
    PyObjectHandle method = PyObject_GetAttrString(servant, const_cast<char*>(fixedName.c_str()));
    if(method.get() == NULL)
    {
        ostringstream ostr;
        ostr << "servant for identity " << Ice::identityToString(current.id) << " does not define operation `"
             << fixedName << "'";
        string str = ostr.str();
        PyErr_Warn(PyExc_RuntimeWarning, const_cast<char*>(str.c_str()));
        Ice::UnknownException ex(__FILE__, __LINE__);
        ex.unknown = str;
        throw ex;
    }

    PyObjectHandle result = PyObject_Call(method.get(), args.get(), NULL);

    //
    // Check for exceptions.
    //
    if(checkDispatchException(outBytes, communicator))
    {
        return false;
    }

    //
    // Marshal the results. If there is more than one value to be returned, then they must be
    // returned in a tuple of the form (result, outParam1, ...).
    //
    Ice::OutputStreamPtr os = Ice::createOutputStream(communicator);
    try
    {
        int i = returnType ? 1 : 0;
        int numResults = static_cast<int>(outParams.size()) + i;
        if(numResults > 1)
        {
            if(!PyTuple_Check(result.get()) || PyTuple_GET_SIZE(result.get()) != numResults)
            {
                ostringstream ostr;
                ostr << "operation `" << fixIdent(name) << "' should return a tuple of length " << numResults;
                string str = ostr.str();
                PyErr_Warn(PyExc_RuntimeWarning, const_cast<char*>(str.c_str()));
                throw Ice::MarshalException(__FILE__, __LINE__);
            }
        }

        ObjectMap objectMap;

        for(ParamInfoList::iterator p = outParams.begin(); p != outParams.end(); ++p, ++i)
        {
            PyObject* arg;
            if(numResults > 1)
            {
                arg = PyTuple_GET_ITEM(result.get(), i);
            }
            else
            {
                arg = result.get();
                assert(outParams.size() == 1);
            }

            if(!(*p)->type->validate(arg))
            {
                // TODO: Provide the parameter name instead
                PyErr_Format(PyExc_AttributeError, "invalid value for out argument %d in operation `%s'", i + 1,
                             const_cast<char*>(name.c_str()));
                return false;
            }
            (*p)->type->marshal(arg, os, &objectMap);
        }

        if(returnType)
        {
            PyObject* res;
            if(numResults > 1)
            {
                res = PyTuple_GET_ITEM(result.get(), 0);
            }
            else
            {
                assert(outParams.size() == 0);
                res = result.get();
            }
            if(!returnType->type->validate(res))
            {
                PyErr_Format(PyExc_AttributeError, "invalid return value for operation `%s'",
                             const_cast<char*>(name.c_str()));
                return false;
            }
            returnType->type->marshal(res, os, &objectMap);
        }

        os->finished(outBytes);
    }
    catch(const AbortMarshaling&)
    {
        throwPythonException();
    }

    return true;
}

bool
IcePy::OperationI::checkDispatchException(std::vector<Ice::Byte>& bytes, const Ice::CommunicatorPtr& communicator)
{
    //
    // Check for exceptions. Return true if we marshaled a user exception, or false if no
    // exception was set. Local exceptions may be thrown.
    //
    PyObject* exType = PyErr_Occurred();
    if(exType)
    {
        //
        // A servant that calls sys.exit() will raise the SystemExit exception.
        // This is normally caught by the interpreter, causing it to exit.
        // However, we have no way to pass this exception to the interpreter,
        // so we act on it directly.
        //
        if(PyErr_GivenExceptionMatches(exType, PyExc_SystemExit))
        {
            handleSystemExit(); // Does not return.
        }

        PyObjectHandle ex = getPythonException(); // Retrieve it before another Python API call clears it.

        PyObject* userExceptionType = lookupType("Ice.UserException");

        if(PyErr_GivenExceptionMatches(exType, userExceptionType))
        {
            //
            // Get the exception's id and Verify that it is legal to be thrown from this operation.
            //
            PyObjectHandle id = PyObject_CallMethod(ex.get(), "ice_id", NULL);
            PyErr_Clear();
            if(id.get() == NULL || !validateException(ex.get()))
            {
                throwPythonException(ex.get()); // Raises UnknownUserException.
            }
            else
            {
                assert(PyString_Check(id.get()));
                char* str = PyString_AS_STRING(id.get());
                ExceptionInfoPtr info = getExceptionInfo(str);
                if(!info)
                {
                    Ice::UnknownUserException e(__FILE__, __LINE__);
                    e.unknown = str;
                    throw e;
                }

                Ice::OutputStreamPtr os = Ice::createOutputStream(communicator);
                ObjectMap objectMap;
                info->marshal(ex.get(), os, &objectMap);
                os->finished(bytes);
                return true;
            }
        }
        else
        {
            throwPythonException(ex.get());
        }
    }

    return false;
}

void
IcePy::OperationI::unmarshalException(const std::vector<Ice::Byte>& bytes, const Ice::CommunicatorPtr& communicator)
{
    Ice::InputStreamPtr is = Ice::createInputStream(communicator, bytes);

    is->readBool(); // usesClasses

    string id = is->readString();
    while(!id.empty())
    {
        ExceptionInfoPtr info = getExceptionInfo(id);
        if(info)
        {
            PyObjectHandle ex = info->unmarshal(is);
            is->finished();

            if(validateException(ex.get()))
            {
                //
                // Set the Python exception.
                //
                assert(PyInstance_Check(ex.get()));
                PyObject* type = (PyObject*)((PyInstanceObject*)ex.get())->in_class;
                Py_INCREF(type);
                PyErr_Restore(type, ex.release(), NULL);
            }
            else
            {
                throwPythonException(ex.get());
            }

            return;
        }
        else
        {
            is->skipSlice();
            id = is->readString();
        }
    }

    //
    // Getting here should be impossible: we can get here only if the
    // sender has marshaled a sequence of type IDs, none of which we
    // have factory for. This means that sender and receiver disagree
    // about the Slice definitions they use.
    //
    throw Ice::UnknownUserException(__FILE__, __LINE__);
}

bool
IcePy::OperationI::validateException(PyObject* ex) const
{
    for(ExceptionInfoList::const_iterator p = exceptions.begin(); p != exceptions.end(); ++p)
    {
        if(PyObject_IsInstance(ex, (*p)->pythonType.get()))
        {
            return true;
        }
    }

    return false;
}

#ifdef WIN32
extern "C"
#endif
static OperationObject*
operationNew(PyObject* /*arg*/)
{
    OperationObject* self = PyObject_New(OperationObject, &OperationType);
    if (self == NULL)
    {
        return NULL;
    }
    self->op = 0;
    return self;
}

#ifdef WIN32
extern "C"
#endif
static int
operationInit(OperationObject* self, PyObject* args, PyObject* /*kwds*/)
{
    char* name;
    PyObject* modeType = lookupType("Ice.OperationMode");
    assert(modeType != NULL);
    PyObject* mode;
    PyObject* inParams;
    PyObject* outParams;
    PyObject* returnType;
    PyObject* exceptions;
    if(!PyArg_ParseTuple(args, "sO!O!O!OO!", &name, modeType, &mode, &PyTuple_Type, &inParams,
                         &PyTuple_Type, &outParams, &returnType, &PyTuple_Type, &exceptions))
    {
        return -1;
    }

    OperationIPtr op = new OperationI;

    op->name = name;

    //
    // mode
    //
    PyObjectHandle modeValue = PyObject_GetAttrString(mode, "value");
    assert(PyInt_Check(modeValue.get()));
    op->mode = (Ice::OperationMode)static_cast<int>(PyInt_AS_LONG(modeValue.get()));

    //
    // inParams
    //
    int i, sz;
    sz = PyTuple_GET_SIZE(inParams);
    for(i = 0; i < sz; ++i)
    {
        ParamInfoPtr param = new ParamInfo;
        param->type = convertType(PyTuple_GET_ITEM(inParams, i));
        assert(param->type);
        op->inParams.push_back(param);
    }

    //
    // outParams
    //
    sz = PyTuple_GET_SIZE(outParams);
    for(i = 0; i < sz; ++i)
    {
        ParamInfoPtr param = new ParamInfo;
        param->type = convertType(PyTuple_GET_ITEM(outParams, i));
        assert(param->type);
        op->outParams.push_back(param);
    }

    //
    // returnType
    //
    if(returnType != Py_None)
    {
        op->returnType = new ParamInfo;
        op->returnType->type = convertType(returnType);
    }

    //
    // exceptions
    //
    sz = PyTuple_GET_SIZE(exceptions);
    for(i = 0; i < sz; ++i)
    {
        PyObject* s = PyTuple_GET_ITEM(exceptions, i);
        assert(PyString_Check(s));
        op->exceptions.push_back(getExceptionInfo(PyString_AS_STRING(s)));
    }

    self->op = new OperationPtr(op);

    return 0;
}

#ifdef WIN32
extern "C"
#endif
static void
operationDealloc(OperationObject* self)
{
    delete self->op;
    PyObject_Del(self);
}

#ifdef WIN32
extern "C"
#endif
static PyObject*
operationInvoke(OperationObject* self, PyObject* args)
{
    assert(PyTuple_GET_SIZE(args) == 2); // Proxy and argument tuple.

    PyObject* pyProxy = PyTuple_GET_ITEM(args, 0);
    Ice::ObjectPrx prx = getProxy(pyProxy);
    Ice::CommunicatorPtr communicator = getProxyCommunicator(pyProxy);

    PyObject* opArgs = PyTuple_GET_ITEM(args, 1);
    assert(PyTuple_Check(opArgs));

    assert(self->op);
    return (*self->op)->invoke(prx, communicator, opArgs);
}

static PyMethodDef OperationMethods[] =
{
    { "invoke", (PyCFunction)operationInvoke, METH_VARARGS, PyDoc_STR("internal function") },
    { NULL, NULL} /* sentinel */
};

namespace IcePy
{

PyTypeObject OperationType =
{
    /* The ob_type field must be initialized in the module init function
     * to be portable to Windows without using C++. */
    PyObject_HEAD_INIT(NULL)
    0,                               /* ob_size */
    "IcePy.Operation",               /* tp_name */
    sizeof(OperationObject),         /* tp_basicsize */
    0,                               /* tp_itemsize */
    /* methods */
    (destructor)operationDealloc,    /* tp_dealloc */
    0,                               /* tp_print */
    0,                               /* tp_getattr */
    0,                               /* tp_setattr */
    0,                               /* tp_compare */
    0,                               /* tp_repr */
    0,                               /* tp_as_number */
    0,                               /* tp_as_sequence */
    0,                               /* tp_as_mapping */
    0,                               /* tp_hash */
    0,                               /* tp_call */
    0,                               /* tp_str */
    0,                               /* tp_getattro */
    0,                               /* tp_setattro */
    0,                               /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,              /* tp_flags */
    0,                               /* tp_doc */
    0,                               /* tp_traverse */
    0,                               /* tp_clear */
    0,                               /* tp_richcompare */
    0,                               /* tp_weaklistoffset */
    0,                               /* tp_iter */
    0,                               /* tp_iternext */
    OperationMethods,                /* tp_methods */
    0,                               /* tp_members */
    0,                               /* tp_getset */
    0,                               /* tp_base */
    0,                               /* tp_dict */
    0,                               /* tp_descr_get */
    0,                               /* tp_descr_set */
    0,                               /* tp_dictoffset */
    (initproc)operationInit,         /* tp_init */
    0,                               /* tp_alloc */
    (newfunc)operationNew,           /* tp_new */
    0,                               /* tp_free */
    0,                               /* tp_is_gc */
};

}

bool
IcePy::initOperation(PyObject* module)
{
    if(PyType_Ready(&OperationType) < 0)
    {
        return false;
    }
    if(PyModule_AddObject(module, "Operation", (PyObject*)&OperationType) < 0)
    {
        return false;
    }

    return true;
}

IcePy::OperationPtr
IcePy::getOperation(PyObject* p)
{
    assert(PyObject_IsInstance(p, (PyObject*)&OperationType) == 1);
    OperationObject* obj = (OperationObject*)p;
    return *obj->op;
}
