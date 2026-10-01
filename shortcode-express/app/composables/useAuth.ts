import type { BaseApiResponse, BusinessAuthModel } from "~/models";

export const useAuth = () => {
    const router = useRouter();
    const { $generateAlertError } = useNuxtApp();

    const businessIdentityCookie = useCookie("rhyolite-identity", {
        maxAge: 60 * 60 * 24,
    });

    const config = useRuntimeConfig();

    // Helper to save permissions to IndexedDB
    const savePermissionsToIndexedDB = (permissions: string[]): Promise<void> => {
        return new Promise((resolve, reject) => {
            if (typeof window === 'undefined') {
                resolve();
                return;
            }
            const request = indexedDB.open('ShortCodeExpressAuthDB', 1);
            request.onupgradeneeded = (event: any) => {
                const db = event.target.result;
                if (!db.objectStoreNames.contains('permissions')) {
                    db.createObjectStore('permissions');
                }
            };
            request.onsuccess = (event: any) => {
                const db = event.target.result;
                const transaction = db.transaction('permissions', 'readwrite');
                const store = transaction.objectStore('permissions');
                store.put(permissions, 'user_permissions');
                transaction.oncomplete = () => resolve();
                transaction.onerror = (e) => reject(e);
            };
            request.onerror = (event) => reject(event);
        });
    };

    // Helper to clear permissions from IndexedDB upon sign out
    const clearPermissionsFromIndexedDB = (): Promise<void> => {
        return new Promise((resolve, reject) => {
            if (typeof window === 'undefined') {
                resolve();
                return;
            }
            const request = indexedDB.open('ShortCodeExpressAuthDB', 1);
            request.onsuccess = (event: any) => {
                const db = event.target.result;
                if (!db.objectStoreNames.contains('permissions')) {
                    resolve();
                    return;
                }
                const transaction = db.transaction('permissions', 'readwrite');
                const store = transaction.objectStore('permissions');
                store.delete('user_permissions');
                transaction.oncomplete = () => resolve();
                transaction.onerror = (e) => reject(e);
            };
            request.onerror = (event) => reject(event);
        });
    };

    const getBusinessAuth = async (payload: Object) => {
        const response = await $fetch<BaseApiResponse<BusinessAuthModel>>(
            "tokenauth/authenticate",
            {
                method: "POST",
                body: payload,
                baseURL: config.public.proxyApiAuthBaseURL,
            }
        );

        if (response.success && response.result) {
            let today = new Date();

            response.result.expiresOn = today
                .setDate(today.getDate() + 1)
                .toString();

            // 1. Isolate the permissions array and save it to IndexedDB
            const permissions = response.result.permissions || [];
            await savePermissionsToIndexedDB(permissions);

            // 2. Create a lightweight clone of the result with an empty permissions array for the cookie
            const cookiePayload = {
                ...response.result,
                permissions: []
            };

            businessIdentityCookie.value = JSON.stringify(cookiePayload);

            await router.push("/");
        }
        return response;
    };

    const signOut = async () => {
        businessIdentityCookie.value = null;
        await clearPermissionsFromIndexedDB();
        window.location.href = "/login";
    };

    return {
        getBusinessAuth,
        signOut,
    };
};