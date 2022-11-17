// RUN: %clangxx -fsycl -fsycl-targets=%sycl_triple -fsycl-device-code-split=per_kernel %s -o %t.out
// RUN: %CPU_RUN_PLACEHOLDER %t.out
// RUN: %GPU_RUN_PLACEHOLDER %t.out

// The test verifies sort API extension.
// Currently it checks the following combinations:
// For number of elements {18, 64}
//   For types {int, char, half, double, Custom}
//     For initial elements values {reversed, random}
//       For comparators {std::less, std::greater}
//         For dimensions {1, 2}
//           For group {work-group, sub-group}
//               joint_sort with
//                   WG size = {16} or {1, 16}
//                   SG size = {8}
//                   elements per WI = 2
//               sort_over_group with
//                   WG size = {number_of_elements} or {1, number_of_elements}
//                   SG size = 8
//                   elements per WI = 1
//
// TODO: Test global memory for temporary storage
// TODO: Consider using USM instead of buffers
//

#include <sycl/sycl.hpp>

#include <algorithm>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

namespace oneapi_exp = sycl::ext::oneapi::experimental;

template <typename...> class KernelNameOverGroup;
template <typename...> class KernelNameJoint;

enum class UseGroupT { SubGroup = true, WorkGroup = false };

// these classes are needed to pass non-type template parameters to KernelName
template <int> class IntWrapper;
template <UseGroupT> class UseGroupWrapper;

class CustomType {
public:
  CustomType(size_t Val) : MVal(Val) {}
  CustomType() : MVal(0) {}

  bool operator<(const CustomType &RHS) const { return MVal < RHS.MVal; }
  bool operator>(const CustomType &RHS) const { return MVal > RHS.MVal; }
  bool operator==(const CustomType &RHS) const { return MVal == RHS.MVal; }

private:
  size_t MVal = 0;
};

constexpr size_t ReqSubGroupSize = 8;

template <UseGroupT UseGroup, int Dims, class T, class Compare>
void RunJointSort(sycl::queue &Q, const std::vector<T> &DataToSort,
                  const Compare &Comp) {

  constexpr size_t WGSize = 16;
  constexpr size_t ElemsPerWI = 2;
  const size_t NumOfElements = DataToSort.size();
  const size_t NumWGs = ((NumOfElements - 1) / WGSize * ElemsPerWI) + 1;

  constexpr size_t NumSubGroups = WGSize / ReqSubGroupSize;

  std::size_t LocalMemorySize = 0;
  if (UseGroup == UseGroupT::SubGroup)
    // Each sub-group needs a piece of memory for sorting
    LocalMemorySize =
        oneapi_exp::default_sorter<Compare>::template memory_required<T>(
            sycl::memory_scope::sub_group, ReqSubGroupSize * ElemsPerWI);
  else
    // A single chunk of memory for each work-group
    LocalMemorySize =
        oneapi_exp::default_sorter<Compare>::template memory_required<T>(
            sycl::memory_scope::work_group, WGSize * ElemsPerWI);

  const sycl::nd_range<Dims> NDRange = [&]() {
    if constexpr (Dims == 1)
      return sycl::nd_range<1>{{WGSize * NumWGs}, {WGSize}};
    else
      return sycl::nd_range<2>{{1, WGSize * NumWGs}, {1, WGSize}};
    static_assert(Dims < 3,
                  "Only one and two dimensional kernels are supported");
  }();

  std::vector<T> DataToSortCase0 = DataToSort;
  std::vector<T> DataToSortCase1 = DataToSort;
  std::vector<T> DataToSortCase2 = DataToSort;

  // Sort data using 3 different versions of joint_sort API
  {
    sycl::buffer<T> BufToSort0(DataToSortCase0.data(), DataToSortCase0.size());
    sycl::buffer<T> BufToSort1(DataToSortCase1.data(), DataToSortCase1.size());
    sycl::buffer<T> BufToSort2(DataToSortCase2.data(), DataToSortCase2.size());

    Q.submit([&](sycl::handler &CGH) {
       auto AccToSort0 = sycl::accessor(BufToSort0, CGH);
       auto AccToSort1 = sycl::accessor(BufToSort1, CGH);
       auto AccToSort2 = sycl::accessor(BufToSort2, CGH);

       // Allocate local memory for all sub-groups in a work-group
       const size_t TotalLocalMemSize = UseGroup == UseGroupT::SubGroup
                                            ? LocalMemorySize * NumSubGroups
                                            : LocalMemorySize;
       sycl::local_accessor<std::byte, 1> Scratch({TotalLocalMemSize}, CGH);

       CGH.parallel_for<KernelNameJoint<IntWrapper<Dims>,
                                        UseGroupWrapper<UseGroup>, T, Compare>>(
           NDRange, [=](sycl::nd_item<Dims> ID) [[intel::reqd_sub_group_size(
                        ReqSubGroupSize)]] {
             auto Group = [&]() {
               if constexpr (UseGroup == UseGroupT::SubGroup)
                 return ID.get_sub_group();
               else
                 return ID.get_group();
             }();

             const size_t WGID = ID.get_group_linear_id();
             const size_t ChunkSize =
                 Group.get_max_local_range().size() * ElemsPerWI;
             const size_t PartID = UseGroup == UseGroupT::SubGroup
                                       ? WGID * Group.get_group_linear_range() +
                                             Group.get_group_linear_id()
                                       : WGID;
             const size_t LocalPartID =
                 UseGroup == UseGroupT::SubGroup
                     ? LocalMemorySize * Group.get_group_linear_id()
                     : 0;

             const size_t StartIdx = ChunkSize * PartID;
             const size_t EndIdx =
                 std::min(ChunkSize * (PartID + 1), NumOfElements);

             // This version of API always sorts in ascending order
             if constexpr (std::is_same_v<Compare, std::less<T>>)
               oneapi_exp::joint_sort(
                   oneapi_exp::group_with_scratchpad(
                       Group,
                       sycl::span{&Scratch[LocalPartID], LocalMemorySize}),
                   &AccToSort0[StartIdx], &AccToSort0[EndIdx]);

             oneapi_exp::joint_sort(
                 oneapi_exp::group_with_scratchpad(
                     Group, sycl::span{&Scratch[LocalPartID], LocalMemorySize}),
                 &AccToSort1[StartIdx], &AccToSort1[EndIdx], Comp);

             oneapi_exp::joint_sort(
                 Group, &AccToSort2[StartIdx], &AccToSort2[EndIdx],
                 oneapi_exp::default_sorter<Compare>(
                     sycl::span{&Scratch[LocalPartID], LocalMemorySize}));
           });
     }).wait_and_throw();
  }

  // Verification
  {
    // Emulate independent sorting of each work-group and/or sub-group
    const size_t ChunkSize = UseGroup == UseGroupT::SubGroup
                                 ? ReqSubGroupSize * ElemsPerWI
                                 : WGSize * ElemsPerWI;
    std::vector<T> DataSorted = DataToSort;
    auto It = DataSorted.begin();
    for (; (It + ChunkSize) < DataSorted.end(); It += ChunkSize)
      std::sort(It, It + ChunkSize, Comp);

    // Sort reminder
    std::sort(It, DataSorted.end(), Comp);

    // This version of API always sorts in ascending order
    if constexpr (std::is_same_v<Compare, std::less<T>>)
      assert(DataToSortCase0 == DataSorted);

    assert(DataToSortCase1 == DataSorted);
    assert(DataToSortCase2 == DataSorted);
  }
}

template <UseGroupT UseGroup, int Dims, class T, class Compare>
void RunSortOVerGroup(sycl::queue &Q, const std::vector<T> &DataToSort,
                      const Compare &Comp) {

  const size_t NumOfElements = DataToSort.size();
  const size_t NumSubGroups = NumOfElements / ReqSubGroupSize + 1;

  const sycl::nd_range<Dims> NDRange = [&]() {
    if constexpr (Dims == 1)
      return sycl::nd_range<1>{{NumOfElements}, {NumOfElements}};
    else
      return sycl::nd_range<2>{{1, NumOfElements}, {1, NumOfElements}};
    static_assert(Dims < 3,
                  "Only one and two dimensional kernels are supported");
  }();

  std::size_t LocalMemorySize = 0;
  if (UseGroup == UseGroupT::SubGroup)
    // Each sub-group needs a piece of memory for sorting
    LocalMemorySize =
        oneapi_exp::default_sorter<Compare>::template memory_required<T>(
            sycl::memory_scope::sub_group, sycl::range<1>{ReqSubGroupSize});
  else
    // A single chunk of memory for each work-group
    LocalMemorySize =
        oneapi_exp::default_sorter<Compare>::template memory_required<T>(
            sycl::memory_scope::work_group, sycl::range<1>{NumOfElements});

  std::vector<T> DataToSortCase0 = DataToSort;
  std::vector<T> DataToSortCase1 = DataToSort;
  std::vector<T> DataToSortCase2 = DataToSort;

  // Sort data using 3 different versions of sort_over_group API
  {
    sycl::buffer<T> BufToSort0(DataToSortCase0.data(), DataToSortCase0.size());
    sycl::buffer<T> BufToSort1(DataToSortCase1.data(), DataToSortCase1.size());
    sycl::buffer<T> BufToSort2(DataToSortCase2.data(), DataToSortCase2.size());

    Q.submit([&](sycl::handler &CGH) {
       auto AccToSort0 = sycl::accessor(BufToSort0, CGH);
       auto AccToSort1 = sycl::accessor(BufToSort1, CGH);
       auto AccToSort2 = sycl::accessor(BufToSort2, CGH);

       // Allocate local memory for all sub-groups in a work-group
       const size_t TotalLocalMemSize = UseGroup == UseGroupT::SubGroup
                                            ? LocalMemorySize * NumSubGroups
                                            : LocalMemorySize;
       sycl::local_accessor<std::byte, 1> Scratch({TotalLocalMemSize}, CGH);

       CGH.parallel_for<KernelNameOverGroup<
           IntWrapper<Dims>, UseGroupWrapper<UseGroup>, T, Compare>>(
           NDRange,
           [=](sycl::nd_item<Dims> id)
               [[intel::reqd_sub_group_size(ReqSubGroupSize)]] {
                 const size_t GlobalLinearID = id.get_global_linear_id();

                 auto Group = [&]() {
                   if constexpr (UseGroup == UseGroupT::SubGroup)
                     return id.get_sub_group();
                   else
                     return id.get_group();
                 }();

                 // Each sub-group should use it's own part of the scratch pad
                 const size_t ScratchShift =
                     UseGroup == UseGroupT::SubGroup
                         ? id.get_sub_group().get_group_linear_id() *
                               LocalMemorySize
                         : 0;
                 std::byte *ScratchPtr = &Scratch[0] + ScratchShift;

                 if constexpr (std::is_same_v<Compare, std::less<T>>)
                   AccToSort0[GlobalLinearID] = oneapi_exp::sort_over_group(
                       oneapi_exp::group_with_scratchpad(
                           Group, sycl::span{ScratchPtr, LocalMemorySize}),
                       AccToSort0[GlobalLinearID]);

                 AccToSort1[GlobalLinearID] = oneapi_exp::sort_over_group(
                     oneapi_exp::group_with_scratchpad(
                         Group, sycl::span{ScratchPtr, LocalMemorySize}),
                     AccToSort1[GlobalLinearID], Comp);

                 AccToSort2[GlobalLinearID] = oneapi_exp::sort_over_group(
                     Group, AccToSort2[GlobalLinearID],
                     oneapi_exp::default_sorter<Compare>(
                         sycl::span{ScratchPtr, LocalMemorySize}));
               });
     }).wait_and_throw();
  }

  // Verification
  {
    // Emulate independent sorting of each work-group/sub-group
    const size_t ChunkSize = UseGroup == UseGroupT::SubGroup
                                 ? ReqSubGroupSize
                                 : NDRange.get_local_range().size();
    std::vector<T> DataSorted = DataToSort;
    auto It = DataSorted.begin();
    for (; (It + ChunkSize) < DataSorted.end(); It += ChunkSize)
      std::sort(It, It + ChunkSize, Comp);

    // Sort reminder
    std::sort(It, DataSorted.end(), Comp);

    if constexpr (std::is_same_v<Compare, std::less<T>>)
      assert(DataToSortCase0 == DataSorted);

    assert(DataToSortCase1 == DataSorted);
    assert(DataToSortCase2 == DataSorted);
  }
}

template <class T> void RunOverType(sycl::queue &Q, size_t DataSize) {
  std::vector<T> DataReversed(DataSize);
  std::vector<T> DataRandom(DataSize);

  std::iota(DataReversed.rbegin(), DataReversed.rend(), (size_t)0);

  // Fill using random numbers
  {
    std::default_random_engine generator;
    std::normal_distribution<float> distribution((10.0), (2.0));
    for (T &Elem : DataRandom)
      Elem = T(distribution(generator));
  }

  auto RunOnDataAndComp = [&](const std::vector<T> &Data,
                              const auto &Comparator) {
    RunSortOVerGroup<UseGroupT::WorkGroup, 1>(Q, Data, Comparator);
    RunSortOVerGroup<UseGroupT::WorkGroup, 2>(Q, Data, Comparator);

    RunSortOVerGroup<UseGroupT::SubGroup, 1>(Q, Data, Comparator);
    RunSortOVerGroup<UseGroupT::SubGroup, 2>(Q, Data, Comparator);

    RunJointSort<UseGroupT::WorkGroup, 1>(Q, Data, Comparator);
    RunJointSort<UseGroupT::WorkGroup, 2>(Q, Data, Comparator);

    RunJointSort<UseGroupT::SubGroup, 1>(Q, Data, Comparator);
    RunJointSort<UseGroupT::SubGroup, 2>(Q, Data, Comparator);
  };

  RunOnDataAndComp(DataReversed, std::greater<T>{});
  RunOnDataAndComp(DataReversed, std::less<T>{});
  RunOnDataAndComp(DataRandom, std::less<T>{});
  RunOnDataAndComp(DataRandom, std::greater<T>{});
}

int main() {
  try {
    sycl::queue Q;

    std::vector<size_t> Sizes{18, 64};

    for (size_t Size : Sizes) {
      RunOverType<std::int32_t>(Q, Size);
      RunOverType<char>(Q, Size);
      if (Q.get_device().has(sycl::aspect::fp16))
        RunOverType<sycl::half>(Q, Size);
      if (Q.get_device().has(sycl::aspect::fp64))
        RunOverType<double>(Q, Size);
      RunOverType<CustomType>(Q, Size);
    }

    std::cout << "Test passed." << std::endl;
  } catch (std::exception &E) {
    std::cout << "Test failed" << std::endl;
    std::cout << E.what() << std::endl;
  }
}
