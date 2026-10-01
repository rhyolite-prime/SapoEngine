
export interface BaseApiResponse<T> {
    result: T;
    success: boolean;
    unAuthorizedRequest: boolean;
    error: Error;
}
  

export interface Error {
    code: boolean;
    details: boolean;
    message: any;
    validationErrors: any;
}


export interface BasePaginationModel<T> {
    pageNo: number;
    totalCount: number;
    totalPages: number;
    data: T;

    lowerBound: number;
    upperBound: number;
}

export type DynamicObject = Record<string, any>;

export interface OpDataBasePaginationModel<T = Record<string, any>> {
    pageNo: number;
    totalCount: number;
    totalPages: number;
    data: T[];
    tableHeaders: string[];
    lowerBound: number;
    upperBound: number;
}
  
export interface Country {
    name: string;
    code: string;
    
}

export interface BusinessAuthModel {
    accessToken: string;
    encryptedAccessToken: string;
    expireInSeconds: number;
    expiresOn: string;
    userId: number
    
}

export interface BaseFilter extends Record<string, any> {
    pageNo: number;
    pageSize: number;
  }
  
