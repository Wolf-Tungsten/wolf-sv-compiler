#include "native/module/methods.hpp"

#include "grhsim/emit/cpu_single_thread.hpp"
#include "grhsim/ir/json.hpp"
#include "grhsim/ir/lower.hpp"
#include "grhsim/ir/pass.hpp"
#include "native/diagnostics/to_python.hpp"
#include "native/session/storage.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace wolvrix::app::pybind
{

    namespace
    {
        PyObject *grhsimFailure(std::string message, std::string action)
        {
            return makeActionResult(
                false,
                singleDiagnostic(wolvrix::lib::diag::DiagnosticKind::Error,
                                 std::move(message), std::move(action)),
                nullptr);
        }

        bool insertSimModule(SessionHandle &session, std::string_view key,
                             wolvrix::lib::grhsim::Module module, bool replace)
        {
            if (replace)
            {
                sessionEraseKey(session, key);
            }
            session.nativeValues.insert_or_assign(
                std::string(key), std::make_unique<SimModuleSlot>(std::move(module)));
            return true;
        }
    } // namespace

    PyObject *py_session_lower_grhsim(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *designKey = nullptr;
        const char *moduleKey = nullptr;
        const char *top = nullptr;
        int replace = 0;
        static const char *kwlist[] = {"session", "design", "module", "top", "replace", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|zp", const_cast<char **>(kwlist),
                                         &sessionObj, &designKey, &moduleKey, &top, &replace))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        auto *design = sessionDesign(*session, designKey);
        if (!design)
        {
            PyErr_Format(PyExc_KeyError, "design key not found: %s", designKey);
            return nullptr;
        }
        std::string insertError;
        if (!ensureSessionInsertable(*session, moduleKey, replace != 0, insertError))
        {
            PyErr_SetString(PyExc_KeyError, insertError.c_str());
            return nullptr;
        }

        wolvrix::lib::diag::Diagnostics diagnostics;
        wolvrix::lib::grhsim::LowerGrhsimOptions options;
        if (top && top[0] != '\0')
        {
            options.top = std::string(top);
        }
        auto module = wolvrix::lib::grhsim::lowerGrhToGrhsim(*design, options, diagnostics);
        const bool success = module.has_value() && !diagnostics.hasError();
        if (success)
        {
            insertSimModule(*session, moduleKey, std::move(*module), replace != 0);
        }
        return makeActionResult(success, diagnostics.messages(),
                                sessionDesignSourceManager(*session, designKey));
    }

    PyObject *py_session_read_grhsim_json_file(PyObject * /*self*/, PyObject *args,
                                               PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *path = nullptr;
        const char *moduleKey = nullptr;
        int replace = 0;
        static const char *kwlist[] = {"session", "path", "module", "replace", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|p", const_cast<char **>(kwlist),
                                         &sessionObj, &path, &moduleKey, &replace))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        std::string insertError;
        if (!ensureSessionInsertable(*session, moduleKey, replace != 0, insertError))
        {
            PyErr_SetString(PyExc_KeyError, insertError.c_str());
            return nullptr;
        }
        std::string contents;
        std::string readError;
        if (!readFileText(path, contents, readError))
        {
            return grhsimFailure(std::move(readError), "read_grhsim_json_file");
        }
        try
        {
            auto module = wolvrix::lib::grhsim::loadJson(contents);
            insertSimModule(*session, moduleKey, std::move(module), replace != 0);
            return makeActionResult(true, {}, nullptr);
        }
        catch (const std::exception &ex)
        {
            return grhsimFailure(ex.what(), "read_grhsim_json_file");
        }
    }

    PyObject *py_session_load_grhsim_json_text(PyObject * /*self*/, PyObject *args,
                                               PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *text = nullptr;
        const char *moduleKey = nullptr;
        int replace = 0;
        static const char *kwlist[] = {"session", "text", "module", "replace", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|p", const_cast<char **>(kwlist),
                                         &sessionObj, &text, &moduleKey, &replace))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        std::string insertError;
        if (!ensureSessionInsertable(*session, moduleKey, replace != 0, insertError))
        {
            PyErr_SetString(PyExc_KeyError, insertError.c_str());
            return nullptr;
        }
        try
        {
            auto module = wolvrix::lib::grhsim::loadJson(text);
            insertSimModule(*session, moduleKey, std::move(module), replace != 0);
            return makeActionResult(true, {}, nullptr);
        }
        catch (const std::exception &ex)
        {
            return grhsimFailure(ex.what(), "load_grhsim_json_text");
        }
    }

    PyObject *py_session_grhsim_json_text(PyObject * /*self*/, PyObject *args,
                                          PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *moduleKey = nullptr;
        int pretty = 1;
        static const char *kwlist[] = {"session", "module", "pretty", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Os|p", const_cast<char **>(kwlist),
                                         &sessionObj, &moduleKey, &pretty))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        auto *module = sessionSimModule(*session, moduleKey);
        if (!module)
        {
            PyErr_Format(PyExc_KeyError, "GRHSIM module key not found: %s", moduleKey);
            return nullptr;
        }
        try
        {
            const std::string json = wolvrix::lib::grhsim::storeJson(*module, pretty != 0);
            return PyUnicode_FromStringAndSize(json.data(), static_cast<Py_ssize_t>(json.size()));
        }
        catch (const std::exception &ex)
        {
            PyErr_SetString(PyExc_RuntimeError, ex.what());
            return nullptr;
        }
    }

    PyObject *py_session_store_grhsim_json(PyObject * /*self*/, PyObject *args,
                                           PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *moduleKey = nullptr;
        const char *output = nullptr;
        int pretty = 1;
        static const char *kwlist[] = {"session", "module", "output", "pretty", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|p", const_cast<char **>(kwlist),
                                         &sessionObj, &moduleKey, &output, &pretty))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        auto *module = sessionSimModule(*session, moduleKey);
        if (!module)
        {
            PyErr_Format(PyExc_KeyError, "GRHSIM module key not found: %s", moduleKey);
            return nullptr;
        }
        try
        {
            const std::filesystem::path path(output);
            if (path.filename().empty())
            {
                return grhsimFailure("output must be a file path", "store_grhsim_json");
            }
            std::error_code error;
            if (!path.parent_path().empty())
            {
                std::filesystem::create_directories(path.parent_path(), error);
                if (error)
                {
                    return grhsimFailure("failed to create output directory: " + error.message(),
                                          "store_grhsim_json");
                }
            }
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                return grhsimFailure("failed to open output file: " + path.string(),
                                      "store_grhsim_json");
            }
            const std::string json = wolvrix::lib::grhsim::storeJson(*module, pretty != 0);
            stream.write(json.data(), static_cast<std::streamsize>(json.size()));
            if (!stream)
            {
                return grhsimFailure("failed to write output file: " + path.string(),
                                      "store_grhsim_json");
            }
            return makeActionResult(true, {}, nullptr);
        }
        catch (const std::exception &ex)
        {
            return grhsimFailure(ex.what(), "store_grhsim_json");
        }
    }

    PyObject *py_session_run_sim_pass(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *passName = nullptr;
        const char *moduleKey = nullptr;
        PyObject *passArgsObj = Py_None;
        int dryrun = 0;
        const char *logLevelText = "warn";
        static const char *kwlist[] = {"session", "name", "module", "args", "dryrun",
                                       "log_level", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|Ops", const_cast<char **>(kwlist),
                                         &sessionObj, &passName, &moduleKey, &passArgsObj,
                                         &dryrun, &logLevelText))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        auto *module = sessionSimModule(*session, moduleKey);
        if (!module)
        {
            PyErr_Format(PyExc_KeyError, "GRHSIM module key not found: %s", moduleKey);
            return nullptr;
        }
        bool ok = false;
        const wolvrix::lib::LogLevel logLevel = parseLogLevel(logLevelText, ok);
        if (!ok)
        {
            PyErr_SetString(PyExc_ValueError, "unknown log_level");
            return nullptr;
        }
        std::vector<std::string> argStorage;
        std::string error;
        if (!parseStringList(passArgsObj, argStorage, error))
        {
            PyErr_SetString(PyExc_ValueError, error.c_str());
            return nullptr;
        }
        std::vector<std::string_view> passArgs;
        passArgs.reserve(argStorage.size());
        for (const std::string &arg : argStorage)
        {
            passArgs.push_back(arg);
        }
        auto pass = wolvrix::lib::grhsim::makeSimPass(passName, passArgs, error);
        if (!pass)
        {
            PyErr_SetString(PyExc_ValueError, error.c_str());
            return nullptr;
        }

        wolvrix::lib::transform::PassDiagnostics diagnostics;
        wolvrix::lib::grhsim::SimPassManager manager;
        manager.options().logLevel = logLevel;
        manager.options().session = dryrun ? nullptr : &session->nativeValues;
        if (logLevel != wolvrix::lib::LogLevel::Off)
        {
            manager.options().logSink = [](wolvrix::lib::LogLevel,
                                           std::string_view tag,
                                           std::string_view message) {
                if (!tag.empty())
                {
                    std::cerr << tag << ": ";
                }
                std::cerr << message << '\n';
            };
        }
        manager.addPass(std::move(pass));
        wolvrix::lib::grhsim::SimPipelineResult result;
        if (dryrun)
        {
            auto copy = *module;
            auto tempSession = cloneSessionStore(session->nativeValues);
            manager.options().session = &tempSession;
            result = manager.run(copy, diagnostics);
        }
        else
        {
            result = manager.run(*module, diagnostics);
        }
        return makePassActionResult(result.success, result.changed,
                                    diagnostics.messages(), nullptr);
    }

    PyObject *py_session_emit_grhsim(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *moduleKey = nullptr;
        const char *backend = nullptr;
        const char *output = nullptr;
        unsigned long long opsPerSourceFile = 50000;
        unsigned long long fixedPointIterationLimit = 100;
        static const char *kwlist[] = {
            "session", "module", "backend", "output", "ops_per_source_file",
            "fixed_point_iteration_limit", nullptr,
        };
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Osss|KK",
                                         const_cast<char **>(kwlist), &sessionObj,
                                         &moduleKey, &backend, &output, &opsPerSourceFile,
                                         &fixedPointIterationLimit))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        auto *module = sessionSimModule(*session, moduleKey);
        if (!module)
        {
            PyErr_Format(PyExc_KeyError, "GRHSIM module key not found: %s", moduleKey);
            return nullptr;
        }
        if (std::string_view(backend) != "cpu")
        {
            PyErr_SetString(PyExc_ValueError, "emit_grhsim currently supports only backend='cpu'");
            return nullptr;
        }
        if (opsPerSourceFile > std::numeric_limits<std::size_t>::max() ||
            fixedPointIterationLimit > std::numeric_limits<uint32_t>::max())
        {
            PyErr_SetString(PyExc_OverflowError, "emit_grhsim numeric option is out of range");
            return nullptr;
        }

        wolvrix::lib::diag::Diagnostics diagnostics;
        wolvrix::lib::grhsim::CpuSingleThreadEmitOptions options;
        options.outputDirectory = output;
        options.opsPerSourceFile = static_cast<std::size_t>(opsPerSourceFile);
        options.fixedPointIterationLimit = static_cast<uint32_t>(fixedPointIterationLimit);
        const auto result = wolvrix::lib::grhsim::emitCpuSingleThread(
            *module, options, diagnostics);
        return makeActionResult(result.success && !diagnostics.hasError(),
                                diagnostics.messages(), nullptr);
    }

    PyObject *py_list_sim_passes(PyObject * /*self*/, PyObject * /*args*/)
    {
        const auto passes = wolvrix::lib::grhsim::availableSimPasses();
        PyObject *list = PyList_New(static_cast<Py_ssize_t>(passes.size()));
        if (!list)
        {
            return nullptr;
        }
        for (std::size_t index = 0; index < passes.size(); ++index)
        {
            PyObject *item = PyUnicode_FromString(passes[index].c_str());
            if (!item)
            {
                Py_DECREF(list);
                return nullptr;
            }
            PyList_SET_ITEM(list, static_cast<Py_ssize_t>(index), item);
        }
        return list;
    }

} // namespace wolvrix::app::pybind
