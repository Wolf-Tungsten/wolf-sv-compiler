#include "native/session/adapters/export.hpp"

#include "core/transform.hpp"

#include <string>

namespace wolvrix::app::pybind
{

    namespace
    {

        PyObject *exportRegToMemReports(const wolvrix::lib::transform::SessionSlot &slot,
                                        std::string_view key,
                                        std::string_view view)
        {
            if (view != "text")
            {
                PyErr_Format(PyExc_ValueError,
                             "unsupported reg-to-mem export view: %s",
                             std::string(view).c_str());
                return nullptr;
            }

            const auto *typed =
                dynamic_cast<const wolvrix::lib::transform::SessionSlotValue<std::string> *>(&slot);
            if (!typed)
            {
                PyErr_Format(PyExc_TypeError,
                             "reg-to-mem session value has unexpected native type: %s",
                             std::string(key).c_str());
                return nullptr;
            }

            return PyUnicode_FromStringAndSize(typed->value.data(),
                                               static_cast<Py_ssize_t>(typed->value.size()));
        }

        const bool kRegToMemExporterRegistered = []() {
            registerSessionNativeExporter("reg-to-mem.reports", exportRegToMemReports);
            return true;
        }();

    } // namespace

} // namespace wolvrix::app::pybind
