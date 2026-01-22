<script setup lang="ts">
import { authClient } from "@/utils/auth-client";
const session = authClient.useSession();
const loggedIn = computed(() => !!session.value?.data);

if (loggedIn.value) {
    await authClient.signOut({
        fetchOptions: {
            onSuccess: () => {
                navigateTo("/");
            },
        },
    });
} else {
    navigateTo("/");
}
</script>