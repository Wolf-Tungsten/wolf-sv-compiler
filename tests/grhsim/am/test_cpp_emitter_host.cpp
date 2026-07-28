#include "grhsim/am/builder.hpp"
#include "grhsim/am/cpp_emitter.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wolvrix::lib::grhsim::am;

namespace {

int fail(const std::string &message) {
  std::cerr << "[grhsim_am_cpp_emitter_host] " << message << '\n';
  return 1;
}

InstructionId addInstruction(LinearProgramBuilder &builder, Opcode opcode,
                             std::initializer_list<VariableId> results,
                             std::initializer_list<VariableId> operands) {
  return builder.addInstruction(
      opcode, std::span<const VariableId>(results.begin(), results.size()),
      std::span<const VariableId>(operands.begin(), operands.size()));
}

InstructionId addInstruction(ScheduledProgramBuilder &builder, Opcode opcode,
                             std::initializer_list<VariableId> results,
                             std::initializer_list<VariableId> operands) {
  return builder.addInstruction(
      opcode, std::span<const VariableId>(results.begin(), results.size()),
      std::span<const VariableId>(operands.begin(), operands.size()));
}

void addBlock(ScheduledProgramBuilder &builder,
              std::initializer_list<InstructionId> instructions) {
  builder.addBlock(std::span<const InstructionId>(instructions.begin(),
                                                  instructions.size()));
}

void setTargets(ScheduledProgramBuilder &builder, InstructionId instruction,
                std::initializer_list<BlockId> targets) {
  builder.setActivationTargets(
      instruction, std::span<const BlockId>(targets.begin(), targets.size()));
}

VariableId addBitConstant(LinearProgramBuilder &builder, TypeId type,
                          uint64_t value) {
  const std::array<uint64_t, 1> words = {value};
  return builder.addVariable(
      type, builder.addConstantInit(builder.addBitLiteral(type, words)));
}

VariableId addBitConstantWords(LinearProgramBuilder &builder, TypeId type,
                               std::span<const uint64_t> words) {
  return builder.addVariable(
      type, builder.addConstantInit(builder.addBitLiteral(type, words)));
}

VariableId addStringConstant(LinearProgramBuilder &builder, TypeId type,
                             std::string_view value) {
  return builder.addVariable(
      type, builder.addConstantInit(builder.addStringLiteral(type, value)));
}

ExecutableModel makeHostModel() {
  LinearProgramBuilder linear;
  const TypeId u1Type = linear.addType(Type::bitVector(1));
  const TypeId u8Type = linear.addType(Type::bitVector(8));
  const TypeId u16Type = linear.addType(Type::bitVector(16));
  const TypeId u32Type = linear.addType(Type::bitVector(32));
  const TypeId s32Type =
      linear.addType(Type::bitVector(32, Signedness::Signed));
  const TypeId s64Type =
      linear.addType(Type::bitVector(64, Signedness::Signed));
  const TypeId u64Type = linear.addType(Type::bitVector(64));
  const TypeId u352Type = linear.addType(Type::bitVector(352));
  const TypeId stringType = linear.addType(Type::string());

  const StringId taskConditionName = linear.addString("task_condition");
  const StringId taskEventName = linear.addString("task_event");
  const StringId dpiConditionName = linear.addString("dpi_condition");
  const StringId dpiEventName = linear.addString("dpi_event");
  const StringId finishConditionName = linear.addString("finish_condition");
  const StringId inputBitName = linear.addString("input_bit");
  const StringId inputValueName = linear.addString("input_value");
  const StringId dpiReturnName = linear.addString("dpi_return");
  const StringId dpiOutputName = linear.addString("dpi_output");
  const StringId dpiFlagName = linear.addString("dpi_flag");
  const StringId jtagReturnName = linear.addString("jtag_return");
  const StringId jtagOutput0Name = linear.addString("jtag_output_0");
  const StringId jtagOutput1Name = linear.addString("jtag_output_1");
  const StringId jtagOutput2Name = linear.addString("jtag_output_2");
  const StringId jtagOutput3Name = linear.addString("jtag_output_3");
  const StringId signedReturnName = linear.addString("signed_return");
  const StringId signedOutputName = linear.addString("signed_output");
  const StringId unsignedOutputName = linear.addString("unsigned_output");
  const StringId pendingClockName = linear.addString("pending_clock");
  const StringId pendingEventName = linear.addString("pending_event");
  const StringId pendingResultName = linear.addString("pending_result");

  const VariableId taskCondition =
      linear.addVariable(u1Type, linear.zeroInit());
  const VariableId taskEvent = linear.addVariable(u1Type, linear.zeroInit());
  const VariableId dpiCondition = linear.addVariable(u1Type, linear.zeroInit());
  const VariableId dpiEvent = linear.addVariable(u1Type, linear.zeroInit());
  const VariableId finishCondition =
      linear.addVariable(u1Type, linear.zeroInit());
  const VariableId inputBit = linear.addVariable(u1Type, linear.zeroInit());
  const VariableId inputValue = linear.addVariable(u32Type, linear.zeroInit());
  const VariableId dpiReturn = linear.addVariable(u32Type, linear.zeroInit());
  const VariableId dpiOutput = linear.addVariable(u16Type, linear.zeroInit());
  const VariableId dpiFlag = linear.addVariable(u1Type, linear.zeroInit());
  const VariableId jtagReturn = linear.addVariable(s32Type, linear.zeroInit());
  const VariableId jtagOutput0 = linear.addVariable(u1Type, linear.zeroInit());
  const VariableId jtagOutput1 = linear.addVariable(u1Type, linear.zeroInit());
  const VariableId jtagOutput2 = linear.addVariable(u1Type, linear.zeroInit());
  const VariableId jtagOutput3 = linear.addVariable(u1Type, linear.zeroInit());
  const VariableId signedReturn =
      linear.addVariable(s64Type, linear.zeroInit());
  const VariableId signedOutput =
      linear.addVariable(s32Type, linear.zeroInit());
  const VariableId unsignedOutput =
      linear.addVariable(u64Type, linear.zeroInit());
  const VariableId pendingClock =
      linear.addVariable(u1Type, linear.zeroInit());
  const VariableId pendingClockOld =
      linear.addVariable(u1Type, linear.undefInit());
  const VariableId pendingEvent =
      linear.addVariable(u1Type, linear.zeroInit());
  const VariableId pendingEventVisible =
      linear.addVariable(u1Type, linear.zeroInit());
  const VariableId pendingGuard =
      linear.addVariable(u1Type, linear.zeroInit());
  const VariableId pendingResult =
      linear.addVariable(u8Type, linear.zeroInit());
  const VariableId pendingStateNext =
      linear.addVariable(u1Type, linear.zeroInit());
  const VariableId pendingState =
      linear.addVariable(u1Type, linear.zeroInit());
  const VariableId pendingStateOld =
      linear.addVariable(u1Type, linear.undefInit());
  const VariableId pendingStateEvent =
      linear.addVariable(u1Type, linear.zeroInit());

  const auto addInputWatch = [&](VariableId input) {
    const TypeId type = linear.view().variable(input).type;
    const VariableId previous = linear.addVariable(type, linear.undefInit());
    const VariableId changed = linear.addVariable(u1Type, linear.zeroInit());
    const InstructionId detector = addInstruction(linear, Opcode::ChangedAny,
                                                  {changed}, {input, previous});
    return std::pair{detector, changed};
  };
  const auto [taskConditionChanged, taskConditionEvent] =
      addInputWatch(taskCondition);
  const auto [taskEventChanged, taskEventEvent] = addInputWatch(taskEvent);
  const auto [dpiConditionChanged, dpiConditionEvent] =
      addInputWatch(dpiCondition);
  const auto [dpiEventChanged, dpiEventEvent] = addInputWatch(dpiEvent);
  const auto [finishConditionChanged, finishConditionEvent] =
      addInputWatch(finishCondition);
  const auto [inputBitChanged, inputBitEvent] = addInputWatch(inputBit);
  const auto [inputValueChanged, inputValueEvent] = addInputWatch(inputValue);
  const auto [pendingClockInputChanged, pendingClockInputEvent] =
      addInputWatch(pendingClock);
  const InstructionId pendingClockChanged = addInstruction(
      linear, Opcode::ChangedPos, {pendingEvent},
      {pendingClock, pendingClockOld});

  const VariableId trueValue = addBitConstant(linear, u1Type, 1);
  const VariableId stderrHandle =
      addBitConstant(linear, u32Type, UINT64_C(0x80000002));
  const VariableId taskFormat = addStringConstant(
      linear, stringType, "fwrite bit=%d value=%08x text=%s %%\n");
  const VariableId wideFormat =
      addStringConstant(linear, stringType, "wide=%d binary=%b hex=%x\n");
  std::string stressFormatText = "stress=";
  for (std::size_t index = 0; index < 2000; ++index) {
    if (index != 0) {
      stressFormatText.push_back(',');
    }
    stressFormatText += "%d";
  }
  stressFormatText.push_back('\n');
  const VariableId stressFormat =
      addStringConstant(linear, stringType, stressFormatText);
  const VariableId finalFormat =
      addStringConstant(linear, stringType, "finalized\n");
  const VariableId dpiText =
      addStringConstant(linear, stringType, "dpi-string");
  const std::array<uint64_t, 6> wideWords = {
      UINT64_MAX, UINT64_MAX, UINT64_MAX,
      UINT64_MAX, UINT64_MAX, UINT64_C(0xffffffff),
  };
  const VariableId wideValue = addBitConstantWords(linear, u352Type, wideWords);
  const VariableId binaryValue = addBitConstant(linear, u8Type, 0xa5);
  const VariableId hexValue = addBitConstant(linear, u16Type, 0xbeef);
  const VariableId signedInput =
      addBitConstant(linear, s64Type, UINT64_C(0xfffffffffffffffb));
  const VariableId pendingArgument = addBitConstant(linear, u8Type, 0x2a);

  const StringId fwriteName = linear.addString("fwrite");
  const InstructionId fwriteCall =
      addInstruction(linear, Opcode::SystemTask, {},
                     {taskCondition, stderrHandle, taskFormat, inputBit,
                      inputValue, dpiText, taskEvent});
  linear.setSystemTaskAttributes(fwriteCall,
                                 SystemTaskAttributes{
                                     .name = fwriteName,
                                     .eventCount = 1,
                                     .schedule = CallSchedule::Normal,
                                 });

  const InstructionId wideFwriteCall =
      addInstruction(linear, Opcode::SystemTask, {},
                     {taskCondition, stderrHandle, wideFormat, wideValue,
                      binaryValue, hexValue, taskEvent});
  linear.setSystemTaskAttributes(wideFwriteCall,
                                 SystemTaskAttributes{
                                     .name = fwriteName,
                                     .eventCount = 1,
                                     .schedule = CallSchedule::Normal,
                                 });

  std::vector<VariableId> stressOperands;
  stressOperands.reserve(2004);
  stressOperands.push_back(taskCondition);
  stressOperands.push_back(stderrHandle);
  stressOperands.push_back(stressFormat);
  for (uint64_t index = 0; index < 2000; ++index) {
    stressOperands.push_back(addBitConstant(linear, u32Type, index));
  }
  stressOperands.push_back(taskEvent);
  const InstructionId stressFwriteCall = linear.addInstruction(
      Opcode::SystemTask, std::span<const VariableId>{}, stressOperands);
  linear.setSystemTaskAttributes(stressFwriteCall,
                                 SystemTaskAttributes{
                                     .name = fwriteName,
                                     .eventCount = 1,
                                     .schedule = CallSchedule::Normal,
                                 });

  const StringId dpiSymbol = linear.addString("am_host_probe");
  const StringId bitParameterName = linear.addString("input_bit");
  const StringId textParameterName = linear.addString("input_text");
  const StringId valueParameterName = linear.addString("input_value");
  const StringId outputParameterName = linear.addString("output_value");
  const StringId flagParameterName = linear.addString("output_flag");
  const std::array<DpiParameter, 5> dpiParameters = {
      DpiParameter{
          .name = bitParameterName,
          .type = u1Type,
          .direction = DpiDirection::Input,
          .abi = DpiAbiKind::Integral,
      },
      DpiParameter{
          .name = textParameterName,
          .type = stringType,
          .direction = DpiDirection::Input,
          .abi = DpiAbiKind::String,
      },
      DpiParameter{
          .name = valueParameterName,
          .type = u32Type,
          .direction = DpiDirection::Input,
          .abi = DpiAbiKind::Integral,
      },
      DpiParameter{
          .name = outputParameterName,
          .type = u16Type,
          .direction = DpiDirection::Output,
          .abi = DpiAbiKind::Integral,
      },
      DpiParameter{
          .name = flagParameterName,
          .type = u1Type,
          .direction = DpiDirection::Output,
          .abi = DpiAbiKind::Integral,
      },
  };
  linear.addDpiImport(dpiSymbol, dpiParameters,
                      DpiReturn{
                          .type = u32Type,
                          .abi = DpiAbiKind::Integral,
                          .present = true,
                      });
  const InstructionId dpiCall =
      addInstruction(linear, Opcode::DpiCall, {dpiReturn, dpiOutput, dpiFlag},
                     {dpiCondition, inputBit, dpiText, inputValue, dpiEvent});
  linear.setDpiCallAttributes(dpiCall, DpiCallAttributes{
                                           .importSymbol = dpiSymbol,
                                           .eventCount = 1,
                                           .eventMode = HostEventMode::Pending,
                                       });

  const StringId jtagSymbol = linear.addString("am_jtag_probe");
  const StringId jtagOutput0Parameter = linear.addString("output_0");
  const StringId jtagOutput1Parameter = linear.addString("output_1");
  const StringId jtagOutput2Parameter = linear.addString("output_2");
  const StringId jtagOutput3Parameter = linear.addString("output_3");
  const StringId jtagInputParameter = linear.addString("input");
  const std::array<DpiParameter, 5> jtagParameters = {
      DpiParameter{
          .name = jtagOutput0Parameter,
          .type = u1Type,
          .direction = DpiDirection::Output,
          .abi = DpiAbiKind::Integral,
      },
      DpiParameter{
          .name = jtagOutput1Parameter,
          .type = u1Type,
          .direction = DpiDirection::Output,
          .abi = DpiAbiKind::Integral,
      },
      DpiParameter{
          .name = jtagOutput2Parameter,
          .type = u1Type,
          .direction = DpiDirection::Output,
          .abi = DpiAbiKind::Integral,
      },
      DpiParameter{
          .name = jtagOutput3Parameter,
          .type = u1Type,
          .direction = DpiDirection::Output,
          .abi = DpiAbiKind::Integral,
      },
      DpiParameter{
          .name = jtagInputParameter,
          .type = u1Type,
          .direction = DpiDirection::Input,
          .abi = DpiAbiKind::Integral,
      },
  };
  linear.addDpiImport(jtagSymbol, jtagParameters,
                      DpiReturn{
                          .type = s32Type,
                          .abi = DpiAbiKind::Integral,
                          .present = true,
                      });
  const InstructionId jtagCall = addInstruction(
      linear, Opcode::DpiCall,
      {jtagReturn, jtagOutput0, jtagOutput1, jtagOutput2, jtagOutput3},
      {dpiCondition, inputBit, dpiEvent});
  linear.setDpiCallAttributes(jtagCall, DpiCallAttributes{
                                            .importSymbol = jtagSymbol,
                                            .eventCount = 1,
                                            .eventMode = HostEventMode::Pending,
                                        });

  const StringId signedSymbol = linear.addString("am_signed_probe");
  const StringId signedInputParameter = linear.addString("signed_input");
  const StringId signedOutputParameter = linear.addString("signed_output");
  const StringId unsignedOutputParameter = linear.addString("unsigned_output");
  const std::array<DpiParameter, 3> signedParameters = {
      DpiParameter{
          .name = signedInputParameter,
          .type = s64Type,
          .direction = DpiDirection::Input,
          .abi = DpiAbiKind::Integral,
      },
      DpiParameter{
          .name = signedOutputParameter,
          .type = s32Type,
          .direction = DpiDirection::Output,
          .abi = DpiAbiKind::Integral,
      },
      DpiParameter{
          .name = unsignedOutputParameter,
          .type = u64Type,
          .direction = DpiDirection::Output,
          .abi = DpiAbiKind::Integral,
      },
  };
  linear.addDpiImport(signedSymbol, signedParameters,
                      DpiReturn{
                          .type = s64Type,
                          .abi = DpiAbiKind::Integral,
                          .present = true,
                      });
  const InstructionId signedCall = addInstruction(
      linear, Opcode::DpiCall, {signedReturn, signedOutput, unsignedOutput},
      {dpiCondition, signedInput, dpiEvent});
  linear.setDpiCallAttributes(signedCall, DpiCallAttributes{
                                              .importSymbol = signedSymbol,
                                              .eventCount = 1,
                                              .eventMode = HostEventMode::Pending,
                                          });

  const StringId pendingSymbol = linear.addString("am_pending_probe");
  const StringId pendingArgumentName = linear.addString("argument");
  const std::array<DpiParameter, 1> pendingParameters = {
      DpiParameter{
          .name = pendingArgumentName,
          .type = u8Type,
          .direction = DpiDirection::Input,
          .abi = DpiAbiKind::Integral,
      },
  };
  linear.addDpiImport(pendingSymbol, pendingParameters,
                      DpiReturn{
                          .type = u8Type,
                          .abi = DpiAbiKind::Integral,
                          .present = true,
                      });
  const InstructionId pendingCall = addInstruction(
      linear, Opcode::DpiCall, {pendingResult},
      {pendingGuard, pendingArgument, pendingEvent});
  linear.setDpiCallAttributes(
      pendingCall, DpiCallAttributes{
                       .importSymbol = pendingSymbol,
                       .eventCount = 1,
                       .eventMode = HostEventMode::Pending,
                   });
  const InstructionId setPendingGuard = addInstruction(
      linear, Opcode::Assign, {pendingGuard}, {pendingEvent});
  const InstructionId copyPendingEvent = addInstruction(
      linear, Opcode::Assign, {pendingEventVisible}, {pendingEvent});
  const InstructionId assignPendingStateNext = addInstruction(
      linear, Opcode::Assign, {pendingStateNext}, {pendingEvent});
  const InstructionId pendingStateWrite =
      addInstruction(linear, Opcode::LatchWrite, {},
                     {pendingStateNext, trueValue, trueValue, pendingState});
  const InstructionId pendingStateChanged =
      addInstruction(linear, Opcode::ChangedAny, {pendingStateEvent},
                     {pendingState, pendingStateOld});

  const StringId finishName = linear.addString("finish");
  const InstructionId finishCall =
      addInstruction(linear, Opcode::SystemTask, {}, {finishCondition});
  linear.setSystemTaskAttributes(finishCall,
                                 SystemTaskAttributes{
                                     .name = finishName,
                                     .eventCount = 0,
                                     .schedule = CallSchedule::Normal,
                                 });

  const InstructionId finalWrite = addInstruction(
      linear, Opcode::SystemTask, {}, {trueValue, stderrHandle, finalFormat});
  linear.setSystemTaskAttributes(finalWrite,
                                 SystemTaskAttributes{
                                     .name = fwriteName,
                                     .eventCount = 0,
                                     .schedule = CallSchedule::Final,
                                 });

  ScheduledProgramBuilder scheduled(linear.finish());
  const auto addHostActivation = [&](VariableId changed) {
    const InstructionId activation =
        addInstruction(scheduled, Opcode::ActForward, {}, {changed});
    setTargets(scheduled, activation, {BlockId{1}});
    return activation;
  };
  const InstructionId activateTaskCondition =
      addHostActivation(taskConditionEvent);
  const InstructionId activateTaskEvent = addHostActivation(taskEventEvent);
  const InstructionId activateDpiCondition =
      addHostActivation(dpiConditionEvent);
  const InstructionId activateDpiEvent = addHostActivation(dpiEventEvent);
  const InstructionId activateFinishCondition =
      addHostActivation(finishConditionEvent);
  const InstructionId activateInputBit = addHostActivation(inputBitEvent);
  const InstructionId activateInputValue = addHostActivation(inputValueEvent);
  const InstructionId activatePending =
      addInstruction(scheduled, Opcode::ActForward, {}, {pendingClockInputEvent});
  setTargets(scheduled, activatePending, {BlockId{2}});
  const InstructionId reactivatePending =
      addInstruction(scheduled, Opcode::ActBackward, {}, {pendingStateEvent});
  setTargets(scheduled, reactivatePending, {BlockId{2}});
  addBlock(scheduled,
           {taskConditionChanged, activateTaskCondition, taskEventChanged,
            activateTaskEvent, dpiConditionChanged, activateDpiCondition,
            dpiEventChanged, activateDpiEvent, finishConditionChanged,
            activateFinishCondition, inputBitChanged, activateInputBit,
            inputValueChanged, activateInputValue, pendingClockInputChanged,
            activatePending});
  addBlock(scheduled, {fwriteCall, wideFwriteCall, stressFwriteCall, dpiCall,
                       jtagCall, signedCall, finishCall, finalWrite});
  // Round 1 captures the edge and raises the guard; the commit Block's
  // tracked state write re-fires the pending Block so that round 2 consumes
  // the call after ChangedPos has recomputed the raw event to zero.
  addBlock(scheduled, {pendingClockChanged, pendingCall, setPendingGuard,
                       copyPendingEvent, assignPendingStateNext});
  addBlock(scheduled,
           {pendingStateWrite, pendingStateChanged, reactivatePending});

  ProgramInterface interface;
  interface.ports = {
      PortBinding{
          .name = taskConditionName,
          .direction = PortDirection::Input,
          .input = taskCondition,
      },
      PortBinding{
          .name = taskEventName,
          .direction = PortDirection::Input,
          .input = taskEvent,
      },
      PortBinding{
          .name = dpiConditionName,
          .direction = PortDirection::Input,
          .input = dpiCondition,
      },
      PortBinding{
          .name = dpiEventName,
          .direction = PortDirection::Input,
          .input = dpiEvent,
      },
      PortBinding{
          .name = finishConditionName,
          .direction = PortDirection::Input,
          .input = finishCondition,
      },
      PortBinding{
          .name = inputBitName,
          .direction = PortDirection::Input,
          .input = inputBit,
      },
      PortBinding{
          .name = inputValueName,
          .direction = PortDirection::Input,
          .input = inputValue,
      },
      PortBinding{
          .name = pendingClockName,
          .direction = PortDirection::Input,
          .input = pendingClock,
      },
      PortBinding{
          .name = dpiReturnName,
          .direction = PortDirection::Output,
          .output = dpiReturn,
      },
      PortBinding{
          .name = dpiOutputName,
          .direction = PortDirection::Output,
          .output = dpiOutput,
      },
      PortBinding{
          .name = dpiFlagName,
          .direction = PortDirection::Output,
          .output = dpiFlag,
      },
      PortBinding{
          .name = jtagReturnName,
          .direction = PortDirection::Output,
          .output = jtagReturn,
      },
      PortBinding{
          .name = jtagOutput0Name,
          .direction = PortDirection::Output,
          .output = jtagOutput0,
      },
      PortBinding{
          .name = jtagOutput1Name,
          .direction = PortDirection::Output,
          .output = jtagOutput1,
      },
      PortBinding{
          .name = jtagOutput2Name,
          .direction = PortDirection::Output,
          .output = jtagOutput2,
      },
      PortBinding{
          .name = jtagOutput3Name,
          .direction = PortDirection::Output,
          .output = jtagOutput3,
      },
      PortBinding{
          .name = signedReturnName,
          .direction = PortDirection::Output,
          .output = signedReturn,
      },
      PortBinding{
          .name = signedOutputName,
          .direction = PortDirection::Output,
          .output = signedOutput,
      },
      PortBinding{
          .name = unsignedOutputName,
          .direction = PortDirection::Output,
          .output = unsignedOutput,
      },
      PortBinding{
          .name = pendingEventName,
          .direction = PortDirection::Output,
          .output = pendingEventVisible,
      },
      PortBinding{
          .name = pendingResultName,
          .direction = PortDirection::Output,
          .output = pendingResult,
      },
  };
  return ExecutableModel{
      .program = scheduled.finish(),
      .interface = std::move(interface),
      .commitBlockBegin = 3,
      .commitBlockEnd = 4,
  };
}

bool writeHarness(const std::filesystem::path &path) {
  std::ofstream harness(path);
  harness << R"CPP(#include "grhsim_HostTop.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
    int dpi_calls = 0;
    int jtag_calls = 0;
    int signed_calls = 0;
    int pending_calls = 0;
    bool dpi_throw_after_outputs = false;
    std::uint8_t seen_bit = 0;
    std::uint8_t seen_jtag_input = 0;
    std::uint32_t seen_value = 0;
    std::int64_t seen_signed_input = 0;
    std::string seen_text;

    std::string expected_host_output()
    {
        std::string output =
            "fwrite bit=1 value=12345670 text=dpi-string %\n";
        output +=
            "wide=9173994463960286046443283581208347763186259956673124494950355357547691504353939232280074212440502746218495 binary=10100101 hex=beef\n";
        output += "stress=";
        for (int index = 0; index < 2000; ++index)
        {
            if (index != 0)
                output.push_back(',');
            output += std::to_string(index);
        }
        output.push_back('\n');
        return output;
    }
}

extern "C" std::uint32_t am_host_probe(std::uint8_t input_bit,
                                        const char *input_text,
                                        std::uint32_t input_value,
                                        std::uint16_t *output_value,
                                        std::uint8_t *output_flag)
{
    ++dpi_calls;
    seen_bit = input_bit;
    seen_value = input_value;
    seen_text = input_text == nullptr ? "<null>" : input_text;
    *output_value = static_cast<std::uint16_t>(UINT16_C(0xb000) |
                                               (input_value & UINT32_C(0xff)));
    *output_flag = input_bit;
    if (dpi_throw_after_outputs)
    {
        dpi_throw_after_outputs = false;
        throw std::runtime_error("injected DPI failure");
    }
    return input_value ^ UINT32_C(0x55aa00ff);
}

extern "C" std::int32_t am_jtag_probe(std::uint8_t *output_0,
                                       std::uint8_t *output_1,
                                       std::uint8_t *output_2,
                                       std::uint8_t *output_3,
                                       std::uint8_t input)
{
    ++jtag_calls;
    seen_jtag_input = input;
    *output_0 = UINT8_C(0);
    *output_1 = UINT8_C(1);
    *output_2 = UINT8_C(2);
    *output_3 = UINT8_C(0xff);
    return -INT32_C(123456789);
}

extern "C" std::int64_t am_signed_probe(std::int64_t input,
                                         std::int32_t *signed_output,
                                         std::uint64_t *unsigned_output)
{
    ++signed_calls;
    seen_signed_input = input;
    *signed_output = -INT32_C(2000000000);
    *unsigned_output = UINT64_C(0xfedcba9876543210);
    return input - INT64_C(7);
}

extern "C" std::uint8_t am_pending_probe(std::uint8_t input)
{
    ++pending_calls;
    return static_cast<std::uint8_t>(input + UINT8_C(1));
}

int main()
{
    GrhSIM_HostTop model;
    model.init();
    model.input_bit = 1;
    model.input_value = UINT32_C(0x12345670);

    std::ostringstream captured;
    std::streambuf *oldStderr = std::cerr.rdbuf(captured.rdbuf());

    model.eval();
    if (dpi_calls != 0 || pending_calls != 0 || !captured.str().empty())
        return 1;

    model.pending_clock = 1;
    model.eval();
    if (pending_calls != 1 || model.pending_event ||
        model.pending_result != UINT8_C(0x2b))
        return 17;
    model.eval();
    if (pending_calls != 1 || model.pending_event ||
        model.pending_result != UINT8_C(0x2b))
        return 18;

    model.task_condition = 1;
    model.eval();
    if (dpi_calls != 0 || !captured.str().empty())
        return 2;

    model.task_condition = 0;
    model.task_event = 1;
    model.eval();
    if (dpi_calls != 0 || !captured.str().empty())
        return 3;

    model.task_condition = 1;
    model.eval();
    const std::string expectedOutput = expected_host_output();
    if (captured.str() != expectedOutput)
        return 4;

    model.task_condition = 0;
    model.task_event = 0;
    model.dpi_condition = 1;
    model.eval();
    if (dpi_calls != 0 || jtag_calls != 0 || signed_calls != 0 ||
        captured.str() != expectedOutput)
        return 5;

    model.dpi_condition = 0;
    model.dpi_event = 1;
    model.eval();
    if (dpi_calls != 0 || jtag_calls != 0 || signed_calls != 0 ||
        captured.str() != expectedOutput)
        return 6;

    model.dpi_condition = 1;
    model.eval();
    if (dpi_calls != 1 || jtag_calls != 1 || signed_calls != 1 ||
        seen_bit != 1 || seen_jtag_input != 1 || seen_signed_input != -5 ||
        seen_value != UINT32_C(0x12345670) || seen_text != "dpi-string")
        return 7;
    const std::uint32_t firstReturn = UINT32_C(0x12345670) ^ UINT32_C(0x55aa00ff);
    const std::uint16_t firstOutput = UINT16_C(0xb070);
    if (model.dpi_return != firstReturn || model.dpi_output != firstOutput ||
        model.dpi_flag != 1)
        return 8;
    if (model.jtag_return !=
            static_cast<std::uint32_t>(-INT32_C(123456789)) ||
        model.jtag_output_0 != 0 || model.jtag_output_1 != 1 ||
        model.jtag_output_2 != 0 || model.jtag_output_3 != 1)
        return 15;
    if (model.signed_return !=
            static_cast<std::uint64_t>(-INT64_C(12)) ||
        model.signed_output !=
            static_cast<std::uint32_t>(-INT32_C(2000000000)) ||
        model.unsigned_output != UINT64_C(0xfedcba9876543210))
        return 16;

    model.input_bit = 0;
    model.input_value = UINT32_C(0x89abcdef);
    dpi_throw_after_outputs = true;
    bool threw = false;
    try
    {
        model.eval();
    }
    catch (const std::runtime_error &)
    {
        threw = true;
    }
    if (!threw || dpi_calls != 2 || jtag_calls != 1 || signed_calls != 1)
        return 9;

    model.dpi_condition = 0;
    model.eval();
    if (model.dpi_return != firstReturn || model.dpi_output != firstOutput ||
        model.dpi_flag != 1)
        return 10;

    model.finish_condition = 1;
    model.eval();
    if (!model.finish_requested() || model.stop_requested() ||
        model.fatal_requested() || model.system_exit_code() != 0)
        return 11;

    const std::string afterFinish = captured.str();
    if (afterFinish != expectedOutput &&
        afterFinish != expectedOutput + "finalized\n")
        return 12;
    model.finalize();
    if (captured.str() != expectedOutput + "finalized\n")
        return 13;
    model.finalize();
    if (captured.str() != expectedOutput + "finalized\n")
        return 14;

    std::cerr.rdbuf(oldStderr);
    return 0;
}
)CPP";
  harness.close();
  return static_cast<bool>(harness);
}

} // namespace

int main() {
  const std::filesystem::path outputDirectory =
      std::filesystem::path(WOLVRIX_GRHSIM_AM_EMIT_ARTIFACT_DIR) /
      "cpp-emitter-host";
  std::filesystem::remove_all(outputDirectory);

  ExecutableModel model = makeHostModel();
  const ValidationResult validation =
      validate(model, ValidationOptions{.level = ValidationLevel::Semantic});
  if (!validation.success()) {
    for (const std::string &error : validation.errors) {
      std::cerr << "[grhsim_am_cpp_emitter_host] validation: " << error << '\n';
    }
    return fail("host/DPI fixture failed semantic validation");
  }
  wolvrix::lib::diag::Diagnostics diagnostics;
  GrhSimAmCppEmitter emitter;
  const GrhSimAmCppResult emitResult =
      emitter.emit(model,
                   GrhSimAmCppOptions{
                       .outputDirectory = outputDirectory,
                       .modelName = "HostTop",
                       .maxOutputFileBytes = 4 * 1024 * 1024,
                       .attributes = {{"blocksPerSource", "1"}},
                   },
                   diagnostics);
  if (!emitResult.success || diagnostics.hasError() ||
      emitResult.artifacts.size() != 8) {
    for (const wolvrix::lib::diag::Diagnostic &diagnostic :
         diagnostics.messages()) {
      std::cerr << "[grhsim_am_cpp_emitter_host] " << diagnostic.message;
      if (!diagnostic.context.empty()) {
        std::cerr << " [" << diagnostic.context << ']';
      }
      std::cerr << '\n';
    }
    return fail("AM C++ emitter failed to generate the host/DPI model");
  }

  const std::filesystem::path harnessPath = outputDirectory / "harness.cpp";
  if (!writeHarness(harnessPath)) {
    return fail("failed to write the generated host/DPI harness");
  }

  const std::string buildCommand = "make -C '" + outputDirectory.string() +
                                   "' CXX=clang++ CXXFLAGS='-std=c++20 -O2'";
  if (std::system(buildCommand.c_str()) != 0) {
    return fail("generated host/DPI AM model failed to compile");
  }

  const std::filesystem::path harnessExecutable = outputDirectory / "harness";
  const std::string harnessCompileCommand =
      "clang++ -std=c++20 -O2 -I'" + outputDirectory.string() + "' '" +
      harnessPath.string() + "' '" +
      (outputDirectory / "libgrhsim_HostTop.a").string() + "' -o '" +
      harnessExecutable.string() + "'";
  if (std::system(harnessCompileCommand.c_str()) != 0) {
    return fail("generated host/DPI AM model harness failed to compile");
  }

  const std::string runCommand = "'" + harnessExecutable.string() + "'";
  if (std::system(runCommand.c_str()) != 0) {
    return fail("generated host/DPI AM model violated host semantics");
  }
  return 0;
}
