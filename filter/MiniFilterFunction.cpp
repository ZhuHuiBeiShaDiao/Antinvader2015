///////////////////////////////////////////////////////////////////////////////
//
// 版权所有 (c) 2011 - 2012
//
// 原始文件名称     : MiniFilterFunction.cpp
// 工程名称         : AntinvaderDriver
// 创建时间         : 2011-04-2
//
//
// 描述             : 关于微过滤驱动通用函数
//
// 更新维护:
//  0000 [2011-04-2]  最初版本.
//
///////////////////////////////////////////////////////////////////////////////

#include "MiniFilterFunction.h"
#include "AntinvaderDef.h"

////////////////////////////////////
//    微过滤驱动操作
////////////////////////////////////

/*---------------------------------------------------------
函数名称:   AllocateAndSwapToNewMdlBuffer
函数描述:   申请新的缓冲区,并拷贝原有数据
输入参数:
            pIoParameterBlock       I/O参数块,包含IRP相关信息
            pulNewBuffer            存放新缓冲指针的空间
            dpMemoryDescribeList    存放Mdl指针空间
            pulOriginalBuffer       存放原始缓冲地址的空间
            pulDataLength           存放数据长度的空间
            ulFlag                  申请的用途
输出参数:
            pulNewBuffer            新缓冲的指针
            dpMemoryDescribeList    Mdl指针
            pulDataLength           数据长度
            pulOriginalBuffer       原地址指针
返回值:
            TRUE    成功
            FALSE   失败
其他:
            申请成功后将会把原先的数据拷贝到新的缓冲中去

            dpMemoryDescribeList
            pulOriginalBuffer
            pulDataLength
            可以置为NULL

更新维护:   2011.4.2     最初版本
            2011.4.3     添加了数据长度返回
---------------------------------------------------------*/
BOOLEAN AllocateAndSwapToNewMdlBuffer(
    __in PFLT_IO_PARAMETER_BLOCK pIoParameterBlock,
    __in PVOLUME_CONTEXT pvcVolumeContext,
    __inout PVOID * ppvNewBuffer,
    __inout_opt PMDL * dpMemoryDescribeList,
    __inout_opt PVOID * ppvOriginalBuffer,
    __inout_opt PULONG pulDataLength,
    __in ALLOCATE_BUFFER_TYPE ulFlag
    )
{
    //FLT_ASSERT(pIoParameterBlock != NULL);

    // 数据长度
    ULONG ulDataLength;

    // 新数据缓冲
    PVOID pvBuffer = NULL;

    // 旧数据缓冲
    PVOID pvOriginalBuffer = NULL;

    // 新Mdl
    PMDL pMemoryDescribeList;

    // 保存数据长度
    switch (ulFlag) {
        //
        // 如果是非缓存读写, 那么读写长度必须对齐.
        //
        case Allocate_BufferRead:
            ulDataLength = (pIoParameterBlock->IrpFlags & IRP_NOCACHE) ?
                (ULONG)ROUND_TO_SIZE(pIoParameterBlock->Parameters.Read.Length, pvcVolumeContext->ulSectorSize) :
                pIoParameterBlock->Parameters.Read.Length;
            break;

        case Allocate_BufferWrite:
            ulDataLength = (pIoParameterBlock->IrpFlags & IRP_NOCACHE) ?
                (ULONG)ROUND_TO_SIZE(pIoParameterBlock->Parameters.Write.Length, pvcVolumeContext->ulSectorSize) :
                pIoParameterBlock->Parameters.Write.Length;
            break;

        case Allocate_BufferDirectoryControl:
            ulDataLength = pIoParameterBlock->Parameters.DirectoryControl.QueryDirectory.Length;
            break;

        default:
            return FALSE;
    }

    //
    // 由于要进行解密操作,在没有权限修改缓冲区内容时需要重新
    // 准备拥有权限的缓冲.如果权限为IoReadAccess即视为需要准
    // 备缓冲.接下来即准备缓冲
    //
    switch (ulFlag) {
        case Allocate_BufferRead:
            pvBuffer = ExAllocatePoolWithTag(
                NonPagedPool,
                (SIZE_T)ulDataLength,
                MEM_TAG_READ_BUFFER);
            break;

        case Allocate_BufferWrite:
            pvBuffer = ExAllocatePoolWithTag(
                NonPagedPool,
                (SIZE_T)ulDataLength,
                MEM_TAG_WRITE_BUFFER);
            break;

        case Allocate_BufferDirectoryControl:
            pvBuffer = ExAllocatePoolWithTag(
                NonPagedPool,
                (SIZE_T)ulDataLength,
                MEM_TAG_DIRECTORY_CONTROL_BUFFER);
            break;

        default:
            pvBuffer = NULL;
            break;
    }

    if (!pvBuffer) {
        // 如果分配失败则返回, 不必跳到末尾.
        return FALSE;
    }

    pMemoryDescribeList = IoAllocateMdl(
        (PVOID)pvBuffer,
        ulDataLength,
        FALSE,      // 没有IRP关联
        FALSE,      // 中间驱动
        NULL        // 没有IRP
        );

    if (!pMemoryDescribeList) {
        FreeAllocatedMdlBuffer(pvBuffer, ulFlag);
        return FALSE;
    }

    //
    //
    // 如果微过滤器使用非分页池中的缓冲来给一个没有设置
    // FLTFL_CALLBACK_DATA_SYSTEM_BUFFER的操作,那么它也
    // 必须用MuBuildMdlForNonpagedPool()并把地址填写到
    // MdlAddress域中.这是因为这么一来,下面的任何过滤器
    // 或者文件系统都不用再尝试去锁定非分页池(可以在构建
    // 的时候使用断言,但是对效率不利),如果提供了一个MDL,
    // 过滤器和文件系统总是可以通过MDL访问缓冲(可以获得
    // 一个系统内存地址来访问它).
    //
    //
    __try {
        // 这个内核函数没有返回值, 只能通过捕获异常来判断是否调用成功.
        MmBuildMdlForNonPagedPool(pMemoryDescribeList);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        FreeAllocatedMdlBuffer(pvBuffer, ulFlag);
        IoFreeMdl(pMemoryDescribeList);
        KdDebugPrint("AllocateAndSwapToNewMdlBuffer(): MmBuildMdlForNonPagedPool() is failed. ExceptionCode() = \n",
            GetExceptionCode());
        return FALSE;
    }

    //
    // 如果有 Mdl 就使用 Mdl 获取地址, 否则直接使用 IRP, 同时
    // 修改 pIoParameterBlock 到新的缓冲.
    //
    switch (ulFlag) {
        case Allocate_BufferRead:
            if (pIoParameterBlock->Parameters.Read.MdlAddress) {
                pvOriginalBuffer = MmGetSystemAddressForMdlSafe(
                    pIoParameterBlock->Parameters.Read.MdlAddress,
                    NormalPagePriority);
            } else {
                pvOriginalBuffer = pIoParameterBlock->Parameters.Read.ReadBuffer;
            }

            pIoParameterBlock->Parameters.Read.ReadBuffer = pvBuffer;
            pIoParameterBlock->Parameters.Read.MdlAddress = pMemoryDescribeList;

            break;

        case Allocate_BufferWrite:
            if (pIoParameterBlock->Parameters.Write.MdlAddress) {
                pvOriginalBuffer = MmGetSystemAddressForMdlSafe(
                    pIoParameterBlock->Parameters.Write.MdlAddress,
                    NormalPagePriority);
            } else {
                pvOriginalBuffer = pIoParameterBlock->Parameters.Write.WriteBuffer;
            }

            pIoParameterBlock->Parameters.Write.WriteBuffer = pvBuffer;
            pIoParameterBlock->Parameters.Write.MdlAddress = pMemoryDescribeList;

            //
            // 拷贝内存到新地址
            //
            RtlCopyMemory(pvBuffer, pvOriginalBuffer, ulDataLength);
            break;

        case Allocate_BufferDirectoryControl:
            pIoParameterBlock->Parameters.DirectoryControl.QueryDirectory.DirectoryBuffer = pvBuffer;
            pIoParameterBlock->Parameters.DirectoryControl.QueryDirectory.MdlAddress = pMemoryDescribeList;
            pvOriginalBuffer = NULL;
            break;
    }

    //
    // 保存返回参数
    //
    if (ppvNewBuffer) {
        *ppvNewBuffer = pvBuffer;
    }
    if (dpMemoryDescribeList) {
        *dpMemoryDescribeList = pMemoryDescribeList;
    }
    if (ppvOriginalBuffer) {
        *ppvOriginalBuffer = pvOriginalBuffer;
    }
    if (pulDataLength) {
        *pulDataLength = ulDataLength;
    }
    return TRUE;
}

/*---------------------------------------------------------
函数名称:   FreeAllocatedMdlBuffer
函数描述:   释放之前申请的缓冲区
输入参数:
            ulNewBuffer             存放新缓冲指针的空间
            ulFlag                  申请的类型
输出参数:
返回值:
其他:
更新维护:   2011.4.2     最初版本
            2011.7.9     增加了无标记判断释放
---------------------------------------------------------*/
VOID FreeAllocatedMdlBuffer(
    __in PVOID pvBuffer,
    __in ALLOCATE_BUFFER_TYPE ulFlag
    )
{
    switch (ulFlag) {
        case Allocate_BufferRead:
            ExFreePoolWithTag(pvBuffer, MEM_TAG_READ_BUFFER);
            break;

        case Allocate_BufferWrite:
            ExFreePoolWithTag(pvBuffer, MEM_TAG_WRITE_BUFFER);
            break;

        case Allocate_BufferDirectoryControl:
            ExFreePoolWithTag(pvBuffer, MEM_TAG_DIRECTORY_CONTROL_BUFFER);
            break;

        default:
            ExFreePool(pvBuffer);
    }
}
