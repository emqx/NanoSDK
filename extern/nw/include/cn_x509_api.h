

//PKI SDK version 1.0.0

#ifndef INC_509M_CN_X509_API_H
#define INC_509M_CN_X509_API_H

#include <stdbool.h>
#include <stdint.h>

#ifdef  __cplusplus
extern "C" {
#endif

/**@enum CertState
* @brief 证书状态
*/

typedef enum{
    AVAILABLE_AND_ACTIVE,
    NO_CERTIFICATE_AVAILABLE,
    DAMAGED,
    AVAILABLE_AND_INACTIVE,
    DEVELOP_CERT
}CertState;

/**@struct CertInfo
* @brief 证书信息
*/
typedef struct{
    CertState certState;
    char certStartTime[32];
    char certEndTime[32];
    char cn[32];
    char serialNumber[64];
    char issuer[128];
    char subject[128];
}CertInfo;

/**
 * @brief 初始化
 *
 * @return 初始化结果
 *
 * @retval true成功，false失败
 */
bool pkiLibraryInit();


/**
* @brief 获取证书
*
* @param[in]	flag	证书类型;1-签名 2-加密
* @param[out]	certData	证书数据
* @param[in,out]	certDataLen	证书数据长度
*
* @return 错误码
* @retval 0-成功 其他-错误
*/
int32_t getIdentityCert(uint32_t flag, uint8_t *cert, uint32_t *certLen);

/**
* @brief 获取证书链
*
* @param[out]	certChainData	证书链数据
* @param[in,out]	certChainDataLen	证书链数据长度
*
* @return 错误码
* @retval 0-成功 其他-错误
*/
int32_t getX509CertChain( uint8_t *certChainData, int32_t* certChainDataLen);


#ifdef  __cplusplus
}
#endif

#endif //INC_509M_CN_X509_API_H
