// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <condition_variable>
#include <future>

#include "debugger.h"
#include "modules.h"


enum ValueKind
{
    ValueIsScope,
    ValueIsClass,
    ValueIsVariable
};

class Evaluator
{
public:

    typedef std::function<HRESULT(mdMethodDef,ICorDebugModule*,ICorDebugType*,ICorDebugValue*,bool,const std::string&)> WalkMembersCallback;
    typedef std::function<HRESULT(ICorDebugILFrame*,ICorDebugValue*,const std::string&)> WalkStackVarsCallback;

    Modules &m_modules;
private:

    ToRelease<ICorDebugFunction> m_pRunClassConstructor;
    ToRelease<ICorDebugFunction> m_pGetTypeHandle;

    std::mutex m_evalMutex;
    std::unordered_map< DWORD, std::promise< std::unique_ptr<ToRelease<ICorDebugValue>> > > m_evalResults;

    HRESULT FollowNested(ICorDebugThread *pThread,
                         ICorDebugILFrame *pILFrame,
                         const std::string &methodClass,
                         const std::vector<std::string> &parts,
                         ICorDebugValue **ppResult);
    HRESULT FollowFields(ICorDebugThread *pThread,
                         ICorDebugILFrame *pILFrame,
                         ICorDebugValue *pValue,
                         ValueKind valueKind,
                         const std::vector<std::string> &parts,
                         int nextPart,
                         ICorDebugValue **ppResult);
    HRESULT GetFieldOrPropertyWithName(ICorDebugThread *pThread,
                                       ICorDebugILFrame *pILFrame,
                                       ICorDebugValue *pInputValue,
                                       ValueKind valueKind,
                                       const std::string &name,
                                       ICorDebugValue **ppResultValue);

    HRESULT WaitEvalResult(ICorDebugThread *pThread,
                           ICorDebugEval *pEval,
                           ICorDebugValue **ppEvalResult);

    HRESULT EvalObjectNoConstructor(
        ICorDebugThread *pThread,
        ICorDebugType *pType,
        ICorDebugValue **ppEvalResult);

    std::future< std::unique_ptr<ToRelease<ICorDebugValue>> > RunEval(
        ICorDebugThread *pThread,
        ICorDebugEval *pEval);

    HRESULT WalkMembers(
        ICorDebugValue *pInputValue,
        ICorDebugThread *pThread,
        ICorDebugILFrame *pILFrame,
        ICorDebugType *pTypeCast,
        WalkMembersCallback cb);

    HRESULT HandleSpecialLocalVar(
        const std::string &localName,
        ICorDebugValue *pLocalValue,
        ICorDebugILFrame *pILFrame,
        std::unordered_set<std::string> &locals,
        WalkStackVarsCallback cb);

    HRESULT HandleSpecialThisParam(
        ICorDebugValue *pThisValue,
        ICorDebugILFrame *pILFrame,
        std::unordered_set<std::string> &locals,
        WalkStackVarsCallback cb);

    HRESULT GetLiteralValue(
        ICorDebugThread *pThread,
        ICorDebugType *pType,
        ICorDebugModule *pModule,
        PCCOR_SIGNATURE pSignatureBlob,
        ULONG sigBlobLength,
        UVCP_CONSTANT pRawValue,
        ULONG rawValueLength,
        ICorDebugValue **ppLiteralValue);

    HRESULT FindType(
        const std::vector<std::string> &parts,
        int &nextPart,
        ICorDebugThread *pThread,
        ICorDebugModule *pModule,
        ICorDebugType **ppType,
        ICorDebugModule **ppModule = nullptr);

    HRESULT ResolveParameters(
        const std::vector<std::string> &params,
        ICorDebugThread *pThread,
        std::vector< ToRelease<ICorDebugType> > &types);

public:

    Evaluator(Modules &modules) : m_modules(modules) {}

    HRESULT RunClassConstructor(ICorDebugThread *pThread, ICorDebugValue *pValue);

    HRESULT EvalFunction(
        ICorDebugThread *pThread,
        ICorDebugFunction *pFunc,
        ICorDebugType *pType, // may be nullptr
        ICorDebugValue *pArgValue, // may be nullptr
        ICorDebugValue **ppEvalResult);

    HRESULT EvalExpr(ICorDebugThread *pThread,
                     ICorDebugFrame *pFrame,
                     const std::string &expression,
                     ICorDebugValue **ppResult);

    bool IsEvalRunning();

    // Should be called by ICorDebugManagedCallback
    void NotifyEvalComplete(ICorDebugThread *pThread, ICorDebugEval *pEval);

    HRESULT ObjectToString(
        ICorDebugThread *pThread,
        ICorDebugValue *pValue,
        std::function<void(const std::string&)> cb
    );

    HRESULT GetType(
        const std::string &typeName,
        ICorDebugThread *pThread,
        ICorDebugType **ppType);

    HRESULT WalkMembers(
        ICorDebugValue *pValue,
        ICorDebugThread *pThread,
        ICorDebugILFrame *pILFrame,
        WalkMembersCallback cb);

    HRESULT WalkStackVars(ICorDebugFrame *pFrame, WalkStackVarsCallback cb);

    void Cleanup();
};

class Breakpoints
{
    struct ManagedBreakpoint {
        uint32_t id;
        CORDB_ADDRESS modAddress;
        mdMethodDef methodToken;
        ULONG32 ilOffset;
        std::string fullname;
        int linenum;
        ToRelease<ICorDebugBreakpoint> breakpoint;
        bool enabled;
        ULONG32 times;

        bool IsResolved() const { return modAddress != 0; }

        ManagedBreakpoint();
        ~ManagedBreakpoint();

        void ToBreakpoint(Breakpoint &breakpoint);

        ManagedBreakpoint(ManagedBreakpoint &&that) = default;
        ManagedBreakpoint(const ManagedBreakpoint &that) = delete;
    };

    Modules &m_modules;
    uint32_t m_nextBreakpointId;
    std::mutex m_breakpointsMutex;
    std::unordered_map<std::string, std::unordered_map<int, ManagedBreakpoint> > m_breakpoints;

    HRESULT ResolveBreakpointInModule(ICorDebugModule *pModule, ManagedBreakpoint &bp);
    HRESULT ResolveBreakpoint(ManagedBreakpoint &bp);

    bool m_stopAtEntry;
    mdMethodDef m_entryPoint;
    ToRelease<ICorDebugFunctionBreakpoint> m_entryBreakpoint;

    HRESULT TrySetupEntryBreakpoint(ICorDebugModule *pModule);
    bool HitEntry(ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint);

public:
    Breakpoints(Modules &modules) :
        m_modules(modules), m_nextBreakpointId(1), m_stopAtEntry(false), m_entryPoint(mdMethodDefNil) {}

    HRESULT HitBreakpoint(
        ICorDebugThread *pThread,
        ICorDebugBreakpoint *pBreakpoint,
        Breakpoint &breakpoint,
        bool &atEntry);

    void DeleteAllBreakpoints();

    void TryResolveBreakpointsForModule(ICorDebugModule *pModule, std::vector<BreakpointEvent> &events);

    void InsertExceptionBreakpoint(const std::string &name, Breakpoint &breakpoint);

    HRESULT SetBreakpoints(
        ICorDebugProcess *pProcess,
        std::string filename,
        const std::vector<int> &lines,
        std::vector<Breakpoint> &breakpoints);

    void SetStopAtEntry(bool stopAtEntry);
};

class Variables
{
    struct VariableReference
    {
        uint32_t variablesReference; // key
        int namedVariables;
        int indexedVariables;

        std::string evaluateName;

        ValueKind valueKind;
        ToRelease<ICorDebugValue> value;
        uint64_t frameId;

        VariableReference(const Variable &variable, uint64_t frameId, ToRelease<ICorDebugValue> value, ValueKind valueKind) :
            variablesReference(variable.variablesReference),
            namedVariables(variable.namedVariables),
            indexedVariables(variable.indexedVariables),
            evaluateName(variable.evaluateName),
            valueKind(valueKind),
            value(std::move(value)),
            frameId(frameId)
        {}

        VariableReference(uint32_t variablesReference, uint64_t frameId, int namedVariables) :
            variablesReference(variablesReference),
            namedVariables(namedVariables),
            indexedVariables(0),
            valueKind(ValueIsScope),
            value(nullptr),
            frameId(frameId)
        {}

        bool IsScope() const { return valueKind == ValueIsScope; }

        VariableReference(VariableReference &&that) = default;
        VariableReference(const VariableReference &that) = delete;
    };

    Evaluator &m_evaluator;

    std::unordered_map<uint32_t, VariableReference> m_variables;
    uint32_t m_nextVariableReference;

    void AddVariableReference(Variable &variable, uint64_t frameId, ICorDebugValue *value, ValueKind valueKind);

    HRESULT GetStackVariables(
        uint64_t frameId,
        ICorDebugThread *pThread,
        ICorDebugFrame *pFrame,
        int start,
        int count,
        std::vector<Variable> &variables);

    HRESULT GetChildren(
        VariableReference &ref,
        ICorDebugThread *pThread,
        ICorDebugFrame *pFrame,
        int start,
        int count,
        std::vector<Variable> &variables);

    struct Member;

    static void FixupInheritedFieldNames(std::vector<Member> &members);

    HRESULT FetchFieldsAndProperties(
        ICorDebugValue *pInputValue,
        ICorDebugThread *pThread,
        ICorDebugILFrame *pILFrame,
        std::vector<Member> &members,
        bool fetchOnlyStatic,
        bool &hasStaticMembers,
        int childStart,
        int childEnd);

    HRESULT GetNumChild(
        ICorDebugValue *pValue,
        unsigned int &numchild,
        bool static_members = false);

public:

    Variables(Evaluator &evaluator) : m_evaluator(evaluator), m_nextVariableReference(1) {}

    int GetNamedVariables(uint32_t variablesReference);

    HRESULT Variables::GetVariables(
        ICorDebugProcess *pProcess,
        uint32_t variablesReference,
        VariablesFilter filter,
        int start,
        int count,
        std::vector<Variable> &variables);

    HRESULT GetScopes(ICorDebugProcess *pProcess, uint64_t frameId, std::vector<Scope> &scopes);

    HRESULT Evaluate(ICorDebugProcess *pProcess, uint64_t frameId, const std::string &expression, Variable &variable);

    void Clear() { m_variables.clear(); m_nextVariableReference = 1; }
};

class ManagedCallback;

class ManagedDebugger : public Debugger
{
private:
    friend class ManagedCallback;
    enum ProcessAttachedState
    {
        ProcessAttached,
        ProcessUnattached
    };
    std::mutex m_processAttachedMutex;
    std::condition_variable m_processAttachedCV;
    ProcessAttachedState m_processAttachedState;

    void NotifyProcessCreated();
    void NotifyProcessExited();
    void WaitProcessExited();
    HRESULT CheckNoProcess();

    std::mutex m_lastStoppedThreadIdMutex;
    int m_lastStoppedThreadId;

    void SetLastStoppedThread(ICorDebugThread *pThread);

    enum StartMethod
    {
        StartNone,
        StartLaunch,
        StartAttach
        //StartAttachForSuspendedLaunch
    } m_startMethod;
    std::string m_execPath;
    std::vector<std::string> m_execArgs;
    bool m_stopAtEntry;

    Modules m_modules;
    Evaluator m_evaluator;
    Breakpoints m_breakpoints;
    Variables m_variables;
    Protocol *m_protocol;
    ToRelease<ManagedCallback> m_managedCallback;
    ICorDebug *m_pDebug;
    ICorDebugProcess *m_pProcess;

    bool m_justMyCode;

    std::mutex m_startupMutex;
    std::condition_variable m_startupCV;
    bool m_startupReady;
    HRESULT m_startupResult;

    PVOID m_unregisterToken;
    DWORD m_processId;
    std::string m_clrPath;

    static VOID StartupCallback(IUnknown *pCordb, PVOID parameter, HRESULT hr);
    HRESULT Startup(IUnknown *punk, int pid);

    void Cleanup();

    static HRESULT DisableAllSteppers(ICorDebugProcess *pProcess);

    HRESULT SetupStep(ICorDebugThread *pThread, StepType stepType);

    HRESULT GetStackTrace(ICorDebugThread *pThread, int startFrame, int levels, std::vector<StackFrame> &stackFrames, int &totalFrames);
    HRESULT GetFrameLocation(ICorDebugFrame *pFrame, int threadId, uint32_t level, StackFrame &stackFrame);

    HRESULT RunProcess(std::string fileExec, std::vector<std::string> execArgs);
    HRESULT AttachToProcess(int pid);
    HRESULT DetachFromProcess();
    HRESULT TerminateProcess();

public:
    ManagedDebugger();
    ~ManagedDebugger() override;

    void SetProtocol(Protocol *protocol) { m_protocol = protocol; }

    bool IsJustMyCode() const override { return m_justMyCode; }
    void SetJustMyCode(bool enable) override { m_justMyCode = enable; }

    HRESULT Initialize() override;
    HRESULT Attach(int pid) override;
    HRESULT Launch(std::string fileExec, std::vector<std::string> execArgs, bool stopAtEntry = false) override;
    HRESULT ConfigurationDone() override;

    HRESULT Disconnect(DisconnectAction action = DisconnectDefault) override;

    int GetLastStoppedThreadId() override;

    HRESULT Continue() override;
    HRESULT Pause() override;
    HRESULT GetThreads(std::vector<Thread> &threads) override;
    HRESULT SetBreakpoints(std::string filename, const std::vector<int> &lines, std::vector<Breakpoint> &breakpoints) override;
    void InsertExceptionBreakpoint(const std::string &name, Breakpoint &breakpoint) override;
    HRESULT GetStackTrace(int threadId, int startFrame, int levels, std::vector<StackFrame> &stackFrames, int &totalFrames) override;
    HRESULT StepCommand(int threadId, StepType stepType) override;
    HRESULT GetScopes(uint64_t frameId, std::vector<Scope> &scopes) override;
    HRESULT GetVariables(uint32_t variablesReference, VariablesFilter filter, int start, int count, std::vector<Variable> &variables) override;
    int GetNamedVariables(uint32_t variablesReference) override;
    HRESULT Evaluate(uint64_t frameId, const std::string &expression, Variable &variable) override;
};
