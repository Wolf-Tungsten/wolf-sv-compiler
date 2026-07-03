#include "native/session/adapters/export.hpp"

#include "core/transform.hpp"
#include "transform/activity_schedule.hpp"

#include <string>

namespace wolvrix::app::pybind
{

    namespace
    {

        PyObject *exportActivityScheduleSupernodeToOps(const wolvrix::lib::transform::SessionSlot &slot,
                                                       std::string_view key,
                                                       std::string_view view)
        {
            if (view != "python")
            {
                PyErr_Format(PyExc_ValueError,
                             "unsupported activity-schedule export view: %s",
                             std::string(view).c_str());
                return nullptr;
            }

            const auto *typed =
                dynamic_cast<const wolvrix::lib::transform::SessionSlotValue<
                    wolvrix::lib::transform::ActivityScheduleSupernodeToOps> *>(&slot);
            if (!typed)
            {
                PyErr_Format(PyExc_TypeError,
                             "activity-schedule session value has unexpected native type: %s",
                             std::string(key).c_str());
                return nullptr;
            }

            const auto &value = typed->value;
            PyObject *outer = PyList_New(static_cast<Py_ssize_t>(value.size()));
            if (!outer)
            {
                return nullptr;
            }

            for (Py_ssize_t superIdx = 0; superIdx < static_cast<Py_ssize_t>(value.size()); ++superIdx)
            {
                const auto &ops = value[static_cast<std::size_t>(superIdx)];
                PyObject *inner = PyList_New(static_cast<Py_ssize_t>(ops.size()));
                if (!inner)
                {
                    Py_DECREF(outer);
                    return nullptr;
                }
                for (Py_ssize_t opIdx = 0; opIdx < static_cast<Py_ssize_t>(ops.size()); ++opIdx)
                {
                    PyObject *item = PyLong_FromUnsignedLong(
                        static_cast<unsigned long>(ops[static_cast<std::size_t>(opIdx)].index));
                    if (!item)
                    {
                        Py_DECREF(inner);
                        Py_DECREF(outer);
                        return nullptr;
                    }
                    PyList_SET_ITEM(inner, opIdx, item);
                }
                PyList_SET_ITEM(outer, superIdx, inner);
            }

            return outer;
        }

        template <typename NestedVector>
        PyObject *exportNestedUint32Vector(const wolvrix::lib::transform::SessionSlot &slot,
                                           std::string_view key,
                                           std::string_view view)
        {
            if (view != "python")
            {
                PyErr_Format(PyExc_ValueError,
                             "unsupported activity-schedule export view: %s",
                             std::string(view).c_str());
                return nullptr;
            }

            const auto *typed =
                dynamic_cast<const wolvrix::lib::transform::SessionSlotValue<NestedVector> *>(&slot);
            if (!typed)
            {
                PyErr_Format(PyExc_TypeError,
                             "activity-schedule session value has unexpected native type: %s",
                             std::string(key).c_str());
                return nullptr;
            }

            const auto &value = typed->value;
            PyObject *outer = PyList_New(static_cast<Py_ssize_t>(value.size()));
            if (!outer)
            {
                return nullptr;
            }

            for (Py_ssize_t outerIdx = 0; outerIdx < static_cast<Py_ssize_t>(value.size()); ++outerIdx)
            {
                const auto &innerVec = value[static_cast<std::size_t>(outerIdx)];
                PyObject *inner = PyList_New(static_cast<Py_ssize_t>(innerVec.size()));
                if (!inner)
                {
                    Py_DECREF(outer);
                    return nullptr;
                }
                for (Py_ssize_t innerIdx = 0; innerIdx < static_cast<Py_ssize_t>(innerVec.size()); ++innerIdx)
                {
                    PyObject *item = PyLong_FromUnsignedLong(
                        static_cast<unsigned long>(innerVec[static_cast<std::size_t>(innerIdx)]));
                    if (!item)
                    {
                        Py_DECREF(inner);
                        Py_DECREF(outer);
                        return nullptr;
                    }
                    PyList_SET_ITEM(inner, innerIdx, item);
                }
                PyList_SET_ITEM(outer, outerIdx, inner);
            }

            return outer;
        }

        PyObject *exportActivityScheduleSupernodeKinds(const wolvrix::lib::transform::SessionSlot &slot,
                                                       std::string_view key,
                                                       std::string_view view)
        {
            if (view != "python")
            {
                PyErr_Format(PyExc_ValueError,
                             "unsupported activity-schedule export view: %s",
                             std::string(view).c_str());
                return nullptr;
            }

            const auto *typed =
                dynamic_cast<const wolvrix::lib::transform::SessionSlotValue<
                    wolvrix::lib::transform::ActivityScheduleSupernodeKinds> *>(&slot);
            if (!typed)
            {
                PyErr_Format(PyExc_TypeError,
                             "activity-schedule session value has unexpected native type: %s",
                             std::string(key).c_str());
                return nullptr;
            }

            PyObject *out = PyList_New(static_cast<Py_ssize_t>(typed->value.size()));
            if (!out)
            {
                return nullptr;
            }
            for (Py_ssize_t idx = 0; idx < static_cast<Py_ssize_t>(typed->value.size()); ++idx)
            {
                const auto kind = typed->value[static_cast<std::size_t>(idx)];
                const char *text =
                    kind == wolvrix::lib::transform::ActivityScheduleSupernodeKind::Commit ? "commit" : "compute";
                PyObject *item = PyUnicode_FromString(text);
                if (!item)
                {
                    Py_DECREF(out);
                    return nullptr;
                }
                PyList_SET_ITEM(out, idx, item);
            }
            return out;
        }

        PyObject *exportActivityScheduleText(const wolvrix::lib::transform::SessionSlot &slot,
                                             std::string_view key,
                                             std::string_view view)
        {
            if (view != "text")
            {
                PyErr_Format(PyExc_ValueError,
                             "unsupported activity-schedule text export view: %s",
                             std::string(view).c_str());
                return nullptr;
            }

            const auto *typed =
                dynamic_cast<const wolvrix::lib::transform::SessionSlotValue<std::string> *>(&slot);
            if (!typed)
            {
                PyErr_Format(PyExc_TypeError,
                             "activity-schedule text session value has unexpected native type: %s",
                             std::string(key).c_str());
                return nullptr;
            }

            return PyUnicode_FromStringAndSize(typed->value.data(),
                                               static_cast<Py_ssize_t>(typed->value.size()));
        }

        const bool kActivityScheduleExporterRegistered = []() {
            registerSessionNativeExporter("activity-schedule.supernode-to-ops",
                                          exportActivityScheduleSupernodeToOps);
            registerSessionNativeExporter("activity-schedule.dag",
                                          exportNestedUint32Vector<wolvrix::lib::transform::ActivityScheduleDag>);
            registerSessionNativeExporter("activity-schedule.supernode-kind",
                                          exportActivityScheduleSupernodeKinds);
            registerSessionNativeExporter("activity-schedule.cbaw-stats",
                                          exportActivityScheduleText);
            return true;
        }();

    } // namespace

} // namespace wolvrix::app::pybind
