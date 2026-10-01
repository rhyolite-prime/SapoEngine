

export const usePermissions = () => {
  const userPermissions = ref<string[]>([]);
  const isPermissionsLoaded = ref(false);

  /**
   * Helper to fetch permissions from the IndexedDB store created by useAuth
   */
  const loadPermissionsFromIndexedDB = (): Promise<string[]> => {
    return new Promise((resolve, reject) => {
      // IndexedDB is only available in the browser environment
      if (typeof window === 'undefined') {
        resolve([]);
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
        
        // Safety check in case the store hasn't been created yet
        if (!db.objectStoreNames.contains('permissions')) {
          resolve([]);
          return;
        }

        const transaction = db.transaction('permissions', 'readonly');
        const store = transaction.objectStore('permissions');
        const getRequest = store.get('user_permissions');

        getRequest.onsuccess = () => {
          // Return the saved array, or an empty array if nothing exists
          resolve(getRequest.result || []);
        };

        getRequest.onerror = (e) => reject(e);
      };

      request.onerror = (event) => reject(event);
    });
  };

  /**
   * Initializes the permissions state on the client side
   */
  const loadPermissions = async () => {
    if (import.meta.client) {
      try {
        userPermissions.value = await loadPermissionsFromIndexedDB();
      } catch (error) {
        console.error("Failed to load user permissions from IndexedDB:", error);
      } finally {
        isPermissionsLoaded.value = true;
      }
    }
  };

  // Trigger the load immediately when the composable is used
  loadPermissions();

  /**
   * Checks if the user has a specific permission
   */
  const hasPermission = (permission: string): boolean => {
    return userPermissions.value.includes(permission);
  };

  /**
   * Checks if the user has ANY of the provided permissions
   */
  const hasAnyPermission = (permissions: string[]): boolean => {
    return permissions.some(permission => hasPermission(permission));
  };

  /**
   * Checks if the user has ALL of the provided permissions
   */
  const hasAllPermissions = (permissions: string[]): boolean => {
    return permissions.every(permission => hasPermission(permission));
  };

  return {
    userPermissions,
    isPermissionsLoaded,
    hasPermission,
    hasAnyPermission,
    hasAllPermissions
  };
};