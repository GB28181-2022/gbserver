import { createApp, ref } from 'vue'
import ElementPlus from 'element-plus'
import 'element-plus/dist/index.css'
import './styles/theme.css'
import App from './App.vue'
import router from './router'
import { getToken, getUser } from './router'

const tokenRef = ref(getToken())
const userRef = ref(getUser())

const app = createApp(App)

app.provide('tokenRef', tokenRef)
app.provide('userRef', userRef)

app.use(router)
app.use(ElementPlus)

app.mount('#app')
